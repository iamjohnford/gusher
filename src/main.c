/*
** Copyright (c) 2013 Peter Yadlowsky <pmy@virginia.edu>
**
** This program is free software ; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation ; either version 3 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY ; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program ; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <libguile.h>
#include <ctype.h>
#include <uuid/uuid.h>
#include <regex.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <poll.h>

#include "postgres.h"
#include "gtime.h"
#include "cache.h"
#include "json.h"
#include "template.h"
#include "gnotify.h"
#include "log.h"
#include "http.h"
#include "butter.h"

#define makesym(s) (scm_from_locale_symbol(s))
#define DEFAULT_PORT 8080
#define BOOT_FILE "boot.scm"
#define COOKIE_KEY "GUSHERID"
#define DEFAULT_MAX_THREADS 32
#define POLL_TIMEOUT 3000
#define POLICE_INTVL 6
#define DEFAULT_GUSHER_ROOT "/var/lib/gusher"

struct handler_entry {
	char *path;
	SCM handler;
	struct handler_entry *link;
	};

typedef struct rframe {
	int sock;
	char ipaddr[32];
	int rport;
	int count;
	struct rframe *next;
	} RFRAME;

static int running;
static const char *prompt = "gusher> ";
static struct handler_entry *handlers = NULL;
char gusher_root[PATH_MAX];
static regex_t cookie_pat;
static const char *hex = "0123456789abcdef";
static SCM threads;
static SCM qmutex;
static SCM pmutex;
static SCM qcondvars;
static SCM scm_handlers;
static SCM query_sym;
static SCM radix10;
static int nthreads = 0;
static int busy_threads = 0;
static int threading;
static int tcount = 0;
static int max_threads = DEFAULT_MAX_THREADS;
static RFRAME *req_pool = NULL;
static RFRAME *req_queue = NULL;
static RFRAME *req_tail = NULL;
static RFRAME one_frame;

static int sorter(const void *a, const void *b) {
	struct handler_entry **e1, **e2;
	e1 = (struct handler_entry **)a;
	e2 = (struct handler_entry **)b;
	return (strlen((*e2)->path) - strlen((*e1)->path));
	}

static SCM set_handler(SCM path, SCM lambda) {
	struct handler_entry *entry, *pt;
	struct handler_entry **list;
	int count, i;
	if (scm_handlers != SCM_EOL) scm_gc_unprotect_object(scm_handlers);
	scm_handlers = scm_acons(path, lambda, scm_handlers);
	scm_gc_protect_object(scm_handlers);
	entry = (struct handler_entry *)malloc(
				sizeof(struct handler_entry));
	entry->path = scm_to_locale_string(path);
	log_msg("set responder for %s\n", entry->path);
	entry->handler = lambda;
	entry->link = handlers;
	handlers = entry;
	count = 0;
	for (pt = handlers; pt != NULL; pt = pt->link) count++;
	list = (struct handler_entry **)malloc(
			sizeof(struct handler_entry *) * count);
	for (i = 0, pt = handlers; pt != NULL; pt = pt->link, i++)
		list[i] = pt;
	qsort((void *)list, count, sizeof(struct handler_entry *), sorter);
	handlers = list[0];
	count--;
	for (i = 0; i < count; i++) list[i]->link = list[i + 1];
	list[count]->link = NULL;
	free(list);
	return SCM_UNSPECIFIED;
	}

static char *downcase(char *buf) {
	char *pt;
	for (pt = buf; *pt; pt++) *pt = tolower(*pt);
	return buf;
	}

static char decode_hex(char *code) {
	int c;
	if (isalpha(code[0])) c = (toupper(code[0]) - 'A' + 10);
	else c = (code[0] - '0');
	c <<= 4;
	if (isalpha(code[1])) c |= (toupper(code[1]) - 'A' + 10);
	else c |= (code[1] - '0');
	return (char)c;
	}

static char *decode_query(char *query) {
	char *get, *put;
	get = put = query;
	while (*get) {
		if (*get == '+') {
			*put++ = ' ';
			get++;
			}
		else if ((*get == '%') && isxdigit(*(get + 1))) {
			*put++ = decode_hex(get + 1);
			get += 3;
			}
		else *put++ = *get++;
		}
	*put = '\0';
	return query;
	}

static SCM parse_query(char *query) {
	SCM list;
	char *next, *mark, *eq;
	list = SCM_EOL;
	mark = query;
	while (1) {
		if ((next = index(mark, '&')) != NULL) {
			*next++ = '\0';
			}
		if ((eq = index(mark, '=')) != NULL) {
			*eq++ = '\0';
			list = scm_acons(makesym(mark),
				scm_from_locale_string(decode_query(eq)), list);
			}
		if (next == NULL) break;
		mark = next;
		}
	scm_remember_upto_here_1(list);
	return list;
	}

static SCM default_not_found(SCM request) {
	SCM resp, headers;
	resp = SCM_EOL;
	resp = scm_cons(scm_from_locale_string("Not Found"), resp);
	headers = SCM_EOL;
	headers = scm_cons(scm_cons(scm_from_locale_string("content-type"),
				scm_from_locale_string("text/plain")),
				headers);
	resp = scm_cons(headers, resp);
	resp = scm_cons(scm_from_locale_string("404 Not Found"), resp);
	scm_remember_upto_here_2(resp, headers);
	scm_remember_upto_here_1(request);
	return resp;
	}

static SCM find_handler(SCM request) {
	char *spath;
	int n;
	SCM path, pair;
	struct handler_entry *pt;
	path = scm_assq_ref(request, makesym("url-path"));
	if (path == SCM_BOOL_F) return SCM_BOOL_F;
	spath = scm_to_locale_string(path);
	scm_remember_upto_here_1(path);
	pair = SCM_EOL;
	for (pt = handlers; pt != NULL; pt = pt->link) {
		n = strlen(pt->path);
		if (strncmp(pt->path, spath, n) == 0) {
			pair = scm_cons(pt->handler, scm_from_locale_string(&spath[n]));
			free(spath);
			return pair;
			}
		}
	free(spath);
	scm_remember_upto_here_2(pair, request);
	return SCM_BOOL_F;
	}

static void send_all(int sock, const char *msg) {
	int sent, len, n;
	sent = 0;
	len = strlen(msg);
	while (sent < len) {
		n = send(sock, msg + sent, len - sent, 0);
		if (n < 0) {
			perror("send!");
			break;
			}
		sent += n;
		}
	}

static SCM simple_http_response(SCM mime_type, SCM content) {
	SCM headers, resp;
	headers = SCM_EOL;
	headers = scm_acons(scm_from_locale_string("content-type"),
						mime_type, headers);
	resp = SCM_EOL;
	resp = scm_cons(content, resp);
	resp = scm_cons(headers, resp);
	resp = scm_cons(scm_from_locale_string("200 OK"), resp);
	scm_remember_upto_here_2(headers, resp);
	scm_remember_upto_here_2(mime_type, content);
	return resp;
	}

static SCM json_http_response(SCM enc_content) {
	SCM headers, resp;
	headers = SCM_EOL;
	headers = scm_acons(scm_from_locale_string("cache-control"),
				scm_from_locale_string("max-age=0, must-revalidate"),
				headers);
	headers = scm_acons(scm_from_locale_string("content-type"),
				scm_from_locale_string("text/json; charset=UTF-8"),
				headers);
	resp = SCM_EOL;
	resp = scm_cons(enc_content, resp);
	resp = scm_cons(headers, resp);
	resp = scm_cons(scm_from_locale_string("200 OK"), resp);
	scm_remember_upto_here_2(headers, resp);
	scm_remember_upto_here_1(enc_content);
	return resp;
	}

static char *session_cookie(SCM request) {
	SCM dough;
	char *buf, *key;
	size_t len;
	regmatch_t match[3];
	dough = scm_assq_ref(request, makesym("cookie"));
	if (dough == SCM_BOOL_F) return NULL;
	key = NULL;
	buf = scm_to_locale_string(dough);
	scm_remember_upto_here_1(dough);
	if (regexec(&cookie_pat, buf, 2, match, 0) == 0) {
		len = match[1].rm_eo - match[1].rm_so;
		key = (char *)malloc(len + 1);
		strncpy(key, &buf[match[1].rm_so], len);
		key[len] = '\0';
		}
	free(buf);
	scm_remember_upto_here_1(request);
	return key;
	}

static char *put_uuid(char *where) {
	uuid_t out;
	int i;
	char *pt;
	uuid_generate(out);
	pt = where;
	for (i = 0; i < 16; i++) {
		*pt++ = hex[((out[i] & 0xf0) >> 4)];
		*pt++ = hex[out[i] & 0x0f];
		}
	*pt = '\0';
	return where;
	}

static SCM uuid_gen(void) {
	char buf[33];
	return scm_from_locale_string(put_uuid(buf));
	}

static RFRAME *get_frame() {
	RFRAME *frame;
	if (!threading) return &one_frame;
	scm_lock_mutex(pmutex);
	if (req_pool == NULL) frame = (RFRAME *)malloc(sizeof(RFRAME));
	else {
		frame = req_pool;
		req_pool = frame->next;
		}
	scm_unlock_mutex(pmutex);
	return frame;
	}

static void release_frame(RFRAME *frame) {
	if (!threading) return;
	scm_lock_mutex(pmutex);
	frame->next = req_pool;
	req_pool = frame;
	scm_unlock_mutex(pmutex);
	return;
	}

static int mygetline(int fd, char *buf, size_t len) {
	int n;
	//n = recv(fd, buf, len, 0);
	n = read(fd, buf, len);
	if (n == 0) {
		log_msg("peer closed connection\n");
		return 0;
		}
	if (n < 0) {
		log_msg("bad recv: %s\n", strerror(errno));
		return 0;
		}
	if (index(buf, '\n') != NULL) return 1;
	log_msg("incoming line too long\n");
	return 0;
	}

static SCM start_request(char *line) {
	SCM request;
	SCM qstring;
	SCM query;
	const char *method;
	char *mark, *pt;
	request = SCM_EOL;
	if (line[0] == 'G') method = "get";
	else method = "post";
	request = scm_acons(makesym("method"), scm_from_locale_string(method),
							request);
	mark = index(line, ' ') + 1;
	*(index(mark, ' ')) = '\0';
	request = scm_acons(makesym("url"), scm_from_locale_string(mark),
							request);
	if ((pt = index(mark, '?')) != NULL) {
		*pt++ = '\0';
		qstring = scm_from_locale_string(pt);
		query = parse_query(pt);
		}
	else {
		query = SCM_EOL;
		qstring = scm_from_locale_string("");
		}
	request = scm_acons(makesym("url-path"), scm_from_locale_string(mark),
							request);
//printf("URL: %s\n", mark);
	request = scm_acons(makesym("query-string"), qstring, request);
	request = scm_acons(query_sym, query, request);
	scm_remember_upto_here_2(query, qstring);
	scm_remember_upto_here_1(request);
	return request;
	}

static SCM dump_request(SCM request) {
	char buf[4096];
	SCM node, pair;
	char *ch;
	pair = SCM_EOL;
	buf[0] = '\0';
	if (threading)
		strcat(buf, "threaded request received\r\n");
	else
		strcat(buf, "sync request received\r\n");
	node = request;
	while (node != SCM_EOL) {
		pair = SCM_CAR(node);
		ch = scm_to_locale_string(scm_symbol_to_string(SCM_CAR(pair)));
		strcat(buf, ch);
		free(ch);
		strcat(buf, ": ");
		if (scm_is_string(SCM_CDR(pair))) {
			ch = scm_to_locale_string(SCM_CDR(pair));
			strcat(buf, ch);
			free(ch);
			}
		else strcat(buf, "NON-STRING");
		strcat(buf, "\n");
		node = SCM_CDR(node);
		}
	scm_remember_upto_here_2(node, pair);
	SCM reply = SCM_EOL;
	reply = scm_cons(scm_from_locale_string(buf), reply);
	SCM headers = SCM_EOL;
	headers = scm_acons(scm_from_locale_string("content-type"),
				scm_from_locale_string("text/plain"), headers);
	reply = scm_cons(headers, reply);
	reply = scm_cons(scm_from_locale_string("404 Not Found"), reply);
	scm_remember_upto_here_2(reply, headers);
	scm_remember_upto_here_1(request);
	return reply;
	}

static void send_headers(int sock, SCM headers) {
	SCM node, pair, val;
	char *hname, *hvalue;
	node = headers;
	pair = val = SCM_EOL;
	while (node != SCM_EOL) {
		pair = SCM_CAR(node);
		hname = scm_to_latin1_string(SCM_CAR(pair));
		send_all(sock, hname);
		free(hname);
		send_all(sock, ": ");
		val = SCM_CDR(pair);
		if (scm_is_string(val))
			hvalue = scm_to_latin1_string(val);
		else if (scm_is_number(val))
			hvalue = scm_to_latin1_string(
						scm_number_to_string(val, radix10));
		else if (scm_is_symbol(val))
			hvalue = scm_to_latin1_string(scm_symbol_to_string(val));
		else hvalue = NULL;
		if (hvalue != NULL) {
			send_all(sock, hvalue);
			free(hvalue);
			}
		send_all(sock, "\r\n");
		node = SCM_CDR(node);
		}
	send_all(sock, "\r\n");
	scm_remember_upto_here_2(node, pair);
	scm_remember_upto_here_1(headers);
	return;
	}

static SCM run_responder(SCM request) {
	SCM cookie_header = SCM_BOOL_F;
	SCM handler;
	char *cookie;
	if ((handler = find_handler(request)) != SCM_BOOL_F) {
		if ((cookie = session_cookie(request)) == NULL) {
			char buf[128];
			cookie = (char *)malloc(33);
			put_uuid(cookie);
			snprintf(buf, sizeof(buf) - 1, "%s=%s; Path=/",
							COOKIE_KEY, cookie);
			buf[sizeof(buf) - 1] = '\0';
			cookie_header = scm_cons(scm_from_latin1_string("set-cookie"),
				scm_from_latin1_string(buf));
			}
		if (get_session(cookie) == SCM_BOOL_F) {
			SCM session;
			session = SCM_EOL;
			session = scm_acons(makesym("_NEW_"), SCM_BOOL_T, session);
			put_session(cookie, session);
			scm_remember_upto_here_1(session);
			}
		request = scm_acons(makesym("session"),
						scm_take_locale_string(cookie), request);
		request = scm_acons(makesym("path-info"),
						SCM_CDR(handler), request);
		}
	SCM reply;
	//if (handler == SCM_BOOL_F) reply = dump_request(request);
	if (handler == SCM_BOOL_F) reply = default_not_found(request);
	else reply = scm_call_1(SCM_CAR(handler), request);
	reply = scm_cons(cookie_header, reply);
	scm_remember_upto_here_1(cookie_header);
	scm_remember_upto_here_2(reply, handler);
	scm_remember_upto_here_1(request);
	return reply;
	}

static void process_request(RFRAME *frame) {
	char buf[4096];
	size_t avail;
	char *mark, *pt, *colon, *status, *body;
	int eoh, sock;
	SCM request;
	sock = frame->sock;
	eoh = 0;
	avail = sizeof(buf);
	request = SCM_EOL;
	while (!eoh) { // build request
		if (!mygetline(sock, buf, avail)) break;
		pt = buf;
		while ((mark = index(pt, '\n')) != NULL) {
			mark++;
			*(mark - 1) = '\0';
			if (*(mark - 2) == '\r') *(mark - 2) = '\0';
			if (strlen(pt) == 0) eoh = 1;
			else if (request == SCM_EOL) // first line of req
				request = start_request(pt);
			else if ((colon = index(pt, ':')) != NULL) {
				*colon++ = '\0';
				while (*colon && isspace(*colon)) colon++;
				request = scm_acons(makesym(downcase(pt)),
						scm_from_locale_string(colon), request);
				}
			pt = mark;
			}
		avail = pt - buf;
		memmove((void *)buf, pt, sizeof(buf) - avail);
		}
	request = scm_acons(makesym("remote-host"),
					scm_from_locale_string(frame->ipaddr), request);
	request = scm_acons(makesym("remote-port"),
					scm_from_signed_integer(frame->rport), request);
	release_frame(frame);
	//SCM reply = dump_request(request);
	//SCM cookie_header = SCM_BOOL_F;
	//-----------------------
	SCM reply = run_responder(request);
	SCM cookie_header = SCM_CAR(reply);
	reply = SCM_CDR(reply);
	//-----------------------
	send_all(sock, "HTTP/1.1 ");
	status = scm_to_latin1_string(SCM_CAR(reply));
	send_all(sock, status);
	free(status);
	send_all(sock, "\r\n");
	reply = SCM_CDR(reply);
	SCM headers = SCM_CAR(reply);
	if (cookie_header != SCM_BOOL_F)
		headers = scm_cons(cookie_header, headers);
	reply = SCM_CDR(reply);
	body = scm_to_utf8_string(SCM_CAR(reply));
	headers = scm_acons(scm_from_latin1_string("content-length"),
						scm_from_int(strlen(body)),
						headers);
	send_headers(sock, headers); // headers
	send_all(sock, body);
	free(body);
	close(sock);
	scm_remember_upto_here_2(request, reply);
	scm_remember_upto_here_2(headers, cookie_header);
	return;
	}

static SCM body_req(void *data) {
	process_request((RFRAME *)data);
	return SCM_BOOL_T;
	}

static const char *err_msg = "HTTP/1.1 500 Internal Server Error\r\nContent-type: text/plain\r\n\r\nwuh-oh! gusher application error\r\n";

static SCM captured_stack = SCM_BOOL_F;

static void backtrace(SCM stack) {
	SCM port, string;
	char *cstring;
	port = scm_open_output_string();
	scm_display_backtrace(stack, port, SCM_BOOL_F, SCM_BOOL_F);
	string = scm_get_output_string(port);
	cstring = scm_to_locale_string(string);
	log_msg("TRACE: %s\n", cstring);
	scm_close_output_port(port);
	free(cstring);
	scm_remember_upto_here_2(port, string);
	return;
	}

static SCM catch_req(void *data, SCM key, SCM params) {
	RFRAME *frame;
	char *buf;
	SCM format;
	frame = (RFRAME *)data;
	format = scm_c_public_ref("guile", "format");
	buf = scm_to_locale_string(scm_call_3(format, SCM_BOOL_F,
		scm_from_locale_string("~s"), key));
	log_msg("ERROR: %s\n", buf);
	free(buf);
	buf = scm_to_locale_string(scm_call_3(format, SCM_BOOL_F,
		scm_from_locale_string("~s"), params));
	log_msg("DETAIL: %s\n", buf);
	backtrace(captured_stack);
	free(buf);
	send_all(frame->sock, err_msg);
	close(frame->sock);
	scm_remember_upto_here_1(format);
	scm_remember_upto_here_2(key, params);
	return SCM_BOOL_T;
	}

static SCM grab_stack(void *data, SCM key, SCM parameters) {
	*((SCM *)data) = scm_make_stack(SCM_BOOL_T, SCM_EOL);
	return SCM_UNSPECIFIED;
	}

static SCM dispatcher(void *data) {
	unsigned long id;
	RFRAME *frame;
	SCM condvar;
	time_t now;
	id = (unsigned long)scm_current_thread();
	log_msg("NEW THREAD %08lx [%d:%d]\n", id, nthreads, max_threads);
	condvar = scm_make_condition_variable();
	scm_permanent_object(condvar);
	if (qcondvars != SCM_EOL) scm_gc_unprotect_object(qcondvars);
	qcondvars = scm_cons(condvar, qcondvars);
	scm_gc_protect_object(qcondvars);
	while (1) {
		SCM res;
		scm_lock_mutex(qmutex);
		if (req_queue == NULL) {
			now = time(NULL) + 1;
			res = scm_timed_wait_condition_variable(condvar, qmutex,
						scm_from_unsigned_integer(now));
			if ((res == SCM_BOOL_F) && (req_queue == NULL)) {
				scm_unlock_mutex(qmutex);
				continue;
				}
			}
		if (req_queue == NULL) {
			scm_unlock_mutex(qmutex);
			continue;
			}
		frame = req_queue;
		req_queue = req_queue->next;
		busy_threads++;
		scm_unlock_mutex(qmutex);
		scm_c_catch(SCM_BOOL_T,
			body_req, (void *)frame,
			catch_req, (void *)frame,
			grab_stack, &captured_stack);
		//scm_lock_mutex(qmutex);
		busy_threads--;
		//scm_unlock_mutex(qmutex);
		}
	return SCM_BOOL_T;
	}

static SCM query_value(SCM request, SCM key) {
	SCM query, value;
	if ((query = scm_assq_ref(request, query_sym)) == SCM_BOOL_F)
		return SCM_BOOL_F;
	value = scm_assq_ref(query, key);
	scm_remember_upto_here_2(query, value);
	scm_remember_upto_here_2(request, key);
	return value;
	}

static SCM query_value_number(SCM request, SCM key) {
	SCM string;
	int n;
	char *buf;
	if ((string = query_value(request, key)) == SCM_BOOL_F) n = 0;
	else {
		n = atoi(buf = scm_to_locale_string(string));
		free(buf);
		}
	return scm_from_signed_integer(n);
	}

static void add_thread() {
	SCM thread;
	if (nthreads >= max_threads) return;
	nthreads++;
	thread = scm_spawn_thread(dispatcher, NULL, NULL, NULL);
	if (threads != SCM_EOL) scm_gc_unprotect_object(threads);
	threads = scm_cons(thread, threads);
	scm_gc_protect_object(threads);
	scm_remember_upto_here_1(thread);
	return;
	}

static void init_env(void) {
	char *here, pats[64];
	struct stat bstat;
	radix10 = scm_from_int(10);
	scm_gc_protect_object(radix10);
	scm_c_define_gsubr("http", 2, 0, 0, set_handler);
	scm_c_define_gsubr("not-found", 1, 0, 0, dump_request);
	scm_c_define_gsubr("uuid-generate", 0, 0, 0, uuid_gen);
	scm_c_define_gsubr("simple-response", 2, 0, 0, simple_http_response);
	scm_c_define_gsubr("json-response", 1, 0, 0, json_http_response);
	scm_c_define_gsubr("query-value", 2, 0, 0, query_value);
	scm_c_define_gsubr("query-value-number", 2, 0, 0, query_value_number);
	query_sym = makesym("query");
	scm_gc_protect_object(query_sym);
	threads = SCM_EOL;
	scm_permanent_object(qmutex = scm_make_mutex());
	scm_permanent_object(pmutex = scm_make_mutex());
	qcondvars = SCM_EOL;
	scm_handlers = SCM_EOL;
	snprintf(pats, sizeof(pats) - 1, "%s=([0-9a-f]+)", COOKIE_KEY);
	pats[sizeof(pats) - 1] = '\0';
	regcomp(&cookie_pat, pats, REG_EXTENDED);
	init_log();
	init_postgres();
	init_time();
	init_cache();
	init_json();
	init_template();
	init_inotify();
	init_http();
	init_butter();
	here = getcwd(NULL, 0);
	if (chdir(gusher_root) == 0) {
		if (stat(BOOT_FILE, &bstat) == 0) {
			log_msg("load %s/%s\n", gusher_root, BOOT_FILE);
			scm_c_primitive_load(BOOT_FILE);
			}
		chdir(here);
		}
	free(here);
	}

static void clear_queues() {
	RFRAME *next;
	while (req_queue != NULL) {
		next = req_queue->next;
		free(req_queue);
		req_queue = next;
		}
	while (req_pool != NULL) {
		next = req_pool->next;
		free(req_pool);
		req_pool = next;
		}
	return;
	}

static void shutdown_env(void) {
	regfree(&cookie_pat);
	clear_queues();
	shutdown_cache();
	shutdown_inotify();
	shutdown_http();
	shutdown_time();
	shutdown_log();
	}

static void signal_handler(int sig) {
	log_msg("aborted by signal %s\n", strsignal(sig));
	running = 0;
	}

static SCM body_proc(void *data) {
	SCM obj;
	char *linebuf = (char *)data;
	obj = scm_c_eval_string(linebuf);
	if (obj == SCM_UNSPECIFIED) return SCM_BOOL_T;
	scm_call_1(scm_c_eval_string("write"), obj);
	scm_call_0(scm_c_eval_string("newline"));
	scm_remember_upto_here_1(obj);
	return SCM_BOOL_T;
	}

static SCM catch_proc(void *data, SCM key, SCM params) {
	scm_call_1(scm_c_eval_string("write"), key);
	scm_call_0(scm_c_eval_string("newline"));
	scm_call_1(scm_c_eval_string("write"), params);
	scm_call_0(scm_c_eval_string("newline"));
	backtrace(captured_stack);
	return SCM_BOOL_T;
	}

static void line_handler(char *line) {
	if (line == NULL) {
		running = 0;
		rl_callback_handler_remove();
		return;
		}
	add_history(line);
	scm_c_catch(SCM_BOOL_T,
			body_proc, (void *)line,
			catch_proc, "line handler",
			grab_stack, &captured_stack);
	free(line);
	return;
	}

static void process_line(int fd) {
	int n;
	char linebuf[1024];
	if (isatty(fd)) {
		rl_callback_read_char();
		return;
		}
	n = read(fd, linebuf, sizeof(linebuf));
	if (n == 0) {
		running = 0;
		return;
		}
	if (n < 0) log_msg("err: %s\n", strerror(errno));
	linebuf[n] = '\0';
	scm_c_catch(SCM_BOOL_T,
			body_proc, linebuf,
			catch_proc, "process line",
			grab_stack, &captured_stack);
	return;
	}

static void enqueue_frame(RFRAME *frame) {
	int need_sig;
	scm_lock_mutex(qmutex);
	frame->next = NULL;
	if ((need_sig = (req_queue == NULL))) req_queue = frame;
	else req_tail->next = frame;
	req_tail = frame;
	if (need_sig) {
		SCM node;
		node = qcondvars;
		while (node != SCM_EOL) {
			scm_signal_condition_variable(SCM_CAR(node));
			node = SCM_CDR(node);
			}
		}
	scm_unlock_mutex(qmutex);
	return;
	}

static void process_http(int sock) {
	socklen_t size;
	RFRAME *frame;
	int fsock;
	struct sockaddr_in client;
	size = sizeof(struct sockaddr_in);
	fsock = accept(sock, (struct sockaddr *)&client, &size);
	if (fsock < 0) {
		log_msg("accept: %s [%d]\n", strerror(errno), errno);
		return;
		}
	frame = get_frame();
	frame->sock = fsock;
	strcpy(frame->ipaddr, inet_ntoa(client.sin_addr));
	frame->rport = ntohs(client.sin_port);
	frame->count = tcount;
	if (threading) {
		if (busy_threads >= nthreads) add_thread();
		enqueue_frame(frame);
		}
	else {
		process_request(frame);
		}
	tcount++;
	return;
	}

static int http_socket(int port) {
	int sock, optval;
	struct sockaddr_in server_addr;    
	sock = socket(AF_INET, SOCK_STREAM, 0);
	optval = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	memset(&server_addr, 0, sizeof(struct sockaddr_in));
        server_addr.sin_family = AF_INET;         
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = INADDR_ANY; 
        if (bind(sock, (struct sockaddr *)&server_addr,
			sizeof(struct sockaddr_in)) != 0) {
				fprintf(stderr, "can't bind: %s\n", strerror(errno));
				return -1;
				}
        if (listen(sock, 5) != 0) {
			fprintf(stderr, "can't listen: %s\n", strerror(errno));
			return -1;
			}
	return sock;
	}

static void police() {
	police_cache();
	return;
	}

int main(int argc, char **argv) {
	struct pollfd polls[3];
	int opt, sock;
	int fdin, nfds;
	int http_port;
	int background;
	time_t mark;
	http_port = DEFAULT_PORT;
	threading = 1;
	background = 0;
	gusher_root[0] = '\0';
	while ((opt = getopt(argc, argv, "sdp:t:")) != -1) {
		switch (opt) {
			case 'p':
				http_port = atoi(optarg);
				break;
			case 't':
				max_threads = atoi(optarg);
				if (max_threads < 1) max_threads = 1;
				break;
			case 'd': // daemon
				background = 1;
				break;
			case 's': // single-threaded
				threading = 0;
				break;
			default:
				log_msg("invalid option: %c", opt);
				exit(1);
			}
		}
	if (background) {
		signal(SIGCHLD, SIG_IGN);
		if (fork() > 0) _exit(0);
		}
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGABRT, signal_handler);
	if (strlen(gusher_root) == 0) {
		strcpy(gusher_root, DEFAULT_GUSHER_ROOT);
		}
	sock = http_socket(http_port);
	if (sock < 0) exit(1);
	scm_init_guile();
	init_env();
	while (optind < argc) {
		log_msg("load %s\n", argv[optind]);
		scm_c_primitive_load(argv[optind]);
		optind++;
		}
	fdin = fileno(stdin);
	polls[0].fd = sock;
	polls[0].events = POLLIN;
	polls[1].fd = inotify_fd;
	polls[1].events = POLLIN;
	polls[2].fd = fdin;
	polls[2].events = POLLIN;
	running = 1;
	if (isatty(fdin))
		rl_callback_handler_install(prompt, line_handler);
	busy_threads = 0;
	if (threading) {
		int n;
		n = sysconf(_SC_NPROCESSORS_ONLN);
		while (n-- > 0) add_thread();
		}
	nfds = (background ? 2 : 3);
	mark = time(NULL) + POLICE_INTVL;
	while (running) {  
		if (time(NULL) >= mark) {
			mark += POLICE_INTVL;
			police();
			}
		if (poll(polls, nfds, POLL_TIMEOUT) < 1) continue;
		//if (running == 0) break; // why?
		if (polls[0].revents & POLLIN) process_http(sock);
		if (polls[1].revents & POLLIN) process_inotify_events();
		if (polls[2].revents & POLLIN) process_line(fdin);
		}
	log_msg("bye!\n");
	close(sock);
	shutdown_env();
	return 0;
	} 
