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

#include "postgres.h"
#include "gtime.h"
#include "cache.h"
#include "json.h"
#include "template.h"
#include "gnotify.h"
#include "log.h"
#include "http.h"

#define makesym(s) (scm_from_locale_symbol(s))
#define addlist(list,item) (list=scm_cons((item),(list)))
#define DEFAULT_PORT 8080
#define BOOT_FILE "boot.scm"
#define COOKIE_KEY "GUSHERID"
#define MAX_THREADS 32

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
static char gusher_root[1024];
static regex_t cookie_pat;
static const char *hex = "0123456789abcdef";
static SCM threads;
static SCM qmutex;
static SCM pmutex;
static SCM qcondvar;
static SCM not_found;
static int nthreads = 0;
static int busy_threads = 0;
static int threading;
static int tcount = 0;
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
	scm_c_define("responders", scm_acons(path, lambda,
		scm_c_eval_string("responders")));
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
	scm_remember_upto_here_2(path, lambda);
	return SCM_UNSPECIFIED;
	}

static char *downcase(char *buf) {
	char *pt;
	for (pt = buf; *pt; pt++) *pt = tolower(*pt);
	return buf;
	}

inline SCM sym_string(const char *symbol, const char *value) {
	return scm_cons(makesym(symbol),
			scm_from_locale_string(value));
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
			addlist(list, sym_string(mark, decode_query(eq)));
			}
		if (next == NULL) break;
		mark = next;
		}
	scm_remember_upto_here_1(list);
	return list;
	}

static void parse_header(char *line, int reqline, SCM *request) {
	SCM value, query, qstring;
	char *mark, *pt;
	if (reqline) {
		if (line[0] == 'G') value = makesym("get");
		else value = makesym("post");
		addlist(*request, scm_cons(makesym("method"), value));
		mark = index(line, ' ') + 1;
		*(index(mark, ' ')) = '\0';
		addlist(*request, sym_string("url", mark));
		if ((pt = index(mark, '?')) != NULL) {
			*pt++ = '\0';
			qstring = scm_from_locale_string(pt);
			query = parse_query(pt);
			}
		else {
			query = SCM_EOL;
			qstring = scm_from_locale_string("");
			}
		addlist(*request, sym_string("url-path", mark));
//printf("URL: %s\n", mark);
		addlist(*request, scm_cons(makesym("query-string"),
				qstring));
		addlist(*request, scm_cons(makesym("query"), query));
		scm_remember_upto_here_2(query, qstring);
		return;
		}
	mark = index(line, ':');
	*mark++ = '\0';
	while (isblank(*mark)) mark++;
	addlist(*request, sym_string(downcase(line), mark));
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
	return resp;
	}

static SCM find_handler(SCM request) {
	char *spath;
	SCM path;
	struct handler_entry *pt;
	path = scm_assq_ref(request, makesym("url-path"));
	if (path == SCM_BOOL_F) return SCM_BOOL_F;
	spath = scm_to_locale_string(path);
	scm_remember_upto_here_1(path);
	for (pt = handlers; pt != NULL; pt = pt->link) {
		if (strncmp(pt->path, spath, strlen(pt->path)) == 0) {
			free(spath);
			return pt->handler;
			}
		}
	free(spath);
	return SCM_BOOL_F;
	}

static void send_all(int sock, const char *msg) {
	int sent, len, n;
	sent = 0;
	len = strlen(msg);
	while (sent < len) {
		n = send(sock, msg + sent, len - sent, 0);
		sent += n;
		}
	}

static char *scm_buf_string(SCM string, char *buf, int size) {
	size_t n;
	n = scm_to_locale_stringbuf(string, buf, size);
	buf[n] = '\0';
	return buf;
	}

static SCM simple_http_response(SCM mime_type, SCM content) {
	SCM headers, resp;
	char *buf, clen[16];
	buf = scm_to_locale_string(content);
	sprintf(clen, "%ld", strlen(buf));
	free(buf);
	headers = SCM_EOL;
	addlist(headers, scm_cons(scm_from_locale_string("content-length"),
						scm_from_locale_string(clen)));
	addlist(headers, scm_cons(scm_from_locale_string("content-type"),
						mime_type));
	resp = SCM_EOL;
	addlist(resp, content);
	addlist(resp, headers);
	addlist(resp, scm_from_locale_string("200 OK"));
	scm_remember_upto_here_2(headers, resp);
	scm_remember_upto_here_2(mime_type, content);
	return resp;
	}

static SCM json_http_response(SCM enc_content) {
	SCM headers, resp;
	char *buf, clen[16];
	buf = scm_to_locale_string(enc_content);
	scm_remember_upto_here_1(enc_content);
	sprintf(clen, "%ld", strlen(buf));
	free(buf);
	headers = SCM_EOL;
	addlist(headers, scm_cons(scm_from_locale_string("content-length"),
						scm_from_locale_string(clen)));
	addlist(headers, scm_cons(scm_from_locale_string("cache-control"),
					scm_from_locale_string("max-age=0, must-revalidate")));
	addlist(headers, scm_cons(scm_from_locale_string("content-type"),
						scm_from_locale_string("text/json")));
	resp = SCM_EOL;
	addlist(resp, enc_content);
	addlist(resp, headers);
	addlist(resp, scm_from_locale_string("200 OK"));
	scm_remember_upto_here_2(headers, resp);
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
	scm_remember_upto_here_2(dough, request);
	if (regexec(&cookie_pat, buf, 2, match, 0) == 0) {
		len = match[1].rm_eo - match[1].rm_so;
		key = (char *)malloc(len + 1);
		strncpy(key, &buf[match[1].rm_so], len);
		key[len] = '\0';
		}
	free(buf);
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
	if (req_pool == NULL) {
		frame = (RFRAME *)malloc(sizeof(RFRAME));
//printf("ALLOC FRAME %08lx\n", (unsigned long)frame);
		}
	else {
		frame = req_pool;
		req_pool = frame->next;
//printf("REUSE FRAME %08lx\n", (unsigned long)frame);
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

static SCM dispatch(void *data) {
	struct rframe *frame;
	char *hname, *hvalue, *body, *status, *cookie;
	int conn, reqline, eoh, n, count;
	SCM request, reply, handler, headers, pair, val, cookie_header;
	char *mark, *pt, buf[65536], sbuf[256];
	frame = (struct rframe *)data;
count = frame->count;
printf("ENTER %d\n", count);
	conn = frame->sock;
	request = SCM_EOL;
	addlist(request, sym_string("remote-host", frame->ipaddr));
	addlist(request, scm_cons(makesym("remote-port"),
		scm_from_signed_integer(frame->rport)));
	free(frame);
	reqline = 1;
	eoh = 0;
	while (!eoh) {
		n = recv(conn, buf, sizeof(buf), 0);
		if (n == 0) {
			log_msg("peer closed connection\n");
			close(conn);
			return SCM_BOOL_F;
			}
		if (n < 0) {
			log_msg("bad recv: %s\n", strerror(errno));
			close(conn);
			return SCM_BOOL_F;
			}
		mark = buf;
		while ((pt = index(mark, '\n')) != NULL) {
			*pt = '\0';
			if (*(pt - 1) == '\r') *(pt - 1) = '\0';
			if (strlen(mark) == 0) {
				eoh = 1;
				break;
				}
			parse_header(mark, reqline, &request);
			reqline = 0;
			mark = pt + 1;
			}
		}
	cookie_header = SCM_EOL;
	if ((handler = find_handler(request)) == SCM_BOOL_F) {
		handler = scm_c_eval_string("not-found");
		}
	else {
		if ((cookie = session_cookie(request)) == NULL) {
			char buf[128];
			cookie = (char *)malloc(33);
			put_uuid(cookie);
			sprintf(buf, "%s=%s; Path=/", COOKIE_KEY, cookie);
			cookie_header = scm_cons(scm_from_locale_string("set-cookie"),
				scm_from_locale_string(buf));
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
		}
/*scm_call_1(scm_c_eval_string("display"), request);
scm_call_0(scm_c_eval_string("newline"));*/
	reply = scm_call_1(handler, request);
	status = scm_buf_string(SCM_CAR(reply), buf, sizeof(buf));
	sprintf(sbuf, "HTTP/1.1 %s\r\n", status);
	send_all(conn, sbuf);
printf("%s", sbuf);
	reply = SCM_CDR(reply);
	headers = SCM_CAR(reply);
	val = pair = SCM_EOL;
	if (cookie_header != SCM_EOL)
		headers = scm_cons(cookie_header, headers);
	while (headers != SCM_EOL) {
		pair = SCM_CAR(headers);
		hname = scm_buf_string(SCM_CAR(pair), buf, sizeof(buf));
		send_all(conn, hname);
		send_all(conn, ": ");
printf("%s: ", hname);
		val = SCM_CDR(pair);
		if (scm_is_string(val))
			hvalue = scm_buf_string(val, buf, sizeof(buf));
		else if (scm_is_number(val))
			hvalue = scm_buf_string(scm_number_to_string(val,
										scm_from_int(10)),
									buf, sizeof(buf));
		else if (scm_is_symbol(val))
			hvalue = scm_buf_string(scm_symbol_to_string(val),
									buf, sizeof(buf));
		else strcpy(hvalue = buf, "foobar");
		send_all(conn, hvalue);
		send_all(conn, "\r\n");
printf("%s\n", hvalue);
		headers = SCM_CDR(headers);
		}
	send_all(conn, "\r\n");
	reply = SCM_CDR(reply);
	body = scm_to_locale_string(SCM_CAR(reply));
	send_all(conn, body);
	free(body);
printf("SEND BODY\n");
	close(conn);
	scm_remember_upto_here_2(request, headers);
	scm_remember_upto_here_2(reply, handler);
	scm_remember_upto_here_1(cookie_header);
	scm_remember_upto_here_2(pair, val);
printf("LEAVE %d\n", count);
	return SCM_BOOL_T;
	}

static SCM err_handler(SCM key, SCM rest) {
	SCM display, newline;
	display = scm_c_eval_string("display");
	newline = scm_c_eval_string("newline");
	scm_call_1(display, rest);
	scm_call_0(newline);
	scm_remember_upto_here_2(display, newline);
	scm_remember_upto_here_2(key, rest);
	return SCM_UNSPECIFIED;
	}

static int mygetline(int fd, char *buf, size_t len) {
	int n;
	n = recv(fd, buf, len, 0);
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
	request = scm_acons(makesym("query"), query, request);
	scm_remember_upto_here_2(query, qstring);
	scm_remember_upto_here_1(request);
	return request;
	}

static SCM dump_request(SCM request) {
	char buf[4096];
	SCM node, pair;
	char *ch;
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
	SCM reply = SCM_EOL;
	reply = scm_cons(scm_from_locale_string(buf), reply);
	SCM headers = SCM_EOL;
	headers = scm_acons(scm_from_locale_string("content-type"),
				scm_from_locale_string("text/plain"), headers);
	reply = scm_cons(headers, reply);
	reply = scm_cons(scm_from_locale_string("200 OK"), reply);
	scm_remember_upto_here_2(reply, headers);
	return reply;
	}

static void send_headers(SCM headers, int sock) {
	SCM node, pair, val;
	char buf[1024];
	char *hname, *hvalue;
	node = headers;
	pair = val = SCM_EOL;
	while (node != SCM_EOL) {
		pair = SCM_CAR(node);
		hname = scm_buf_string(SCM_CAR(pair), buf, sizeof(buf));
		send_all(sock, hname);
		send_all(sock, ": ");
//printf("%s: ", hname);
		val = SCM_CDR(pair);
		if (scm_is_string(val))
			hvalue = scm_buf_string(val, buf, sizeof(buf));
		else if (scm_is_number(val))
			hvalue = scm_buf_string(scm_number_to_string(val,
										scm_from_int(10)),
									buf, sizeof(buf));
		else if (scm_is_symbol(val))
			hvalue = scm_buf_string(scm_symbol_to_string(val),
									buf, sizeof(buf));
		else strcpy(hvalue = buf, "foobar");
		send_all(sock, hvalue);
		send_all(sock, "\r\n");
//printf("%s\n", hvalue);
		node = SCM_CDR(node);
		}
	send_all(sock, "\r\n");
	scm_remember_upto_here_2(node, pair);
	return;
	}

static SCM run_responder(SCM request) {
	SCM cookie_header = SCM_BOOL_F;
	SCM handler;
	//char *cookie;
	if ((handler = find_handler(request)) == SCM_BOOL_F) {
		handler = not_found;
		}
	/*else {
		if ((cookie = session_cookie(request)) == NULL) {
			char buf[128];
			cookie = (char *)malloc(33);
			put_uuid(cookie);
			sprintf(buf, "%s=%s; Path=/", COOKIE_KEY, cookie);
			cookie_header = scm_cons(scm_from_locale_string("set-cookie"),
				scm_from_locale_string(buf));
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
		}*/
	SCM reply = scm_call_1(handler, request);
	reply = scm_cons(cookie_header, reply);
	scm_remember_upto_here_1(request);
	scm_remember_upto_here_1(cookie_header);
	scm_remember_upto_here_1(reply);
	return reply;
	}

static void process_request(RFRAME *frame) {
	char buf[4096], sbuf[128];
	size_t avail;
	char *mark, *pt, *colon;
	int eoh, sock;
	SCM request;
	sock = frame->sock;
	avail = sizeof(buf);
	eoh = 0;
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
	sprintf(sbuf, "HTTP/1.1 %s\r\n", 
				scm_buf_string(SCM_CAR(reply), buf, sizeof(buf)));
	send_all(sock, sbuf); // status
//printf("%s", sbuf);
	reply = SCM_CDR(reply);
	SCM headers = SCM_CAR(reply);
	if (cookie_header != SCM_BOOL_F)
		headers = scm_cons(cookie_header, headers);
	send_headers(headers, sock); // headers
	reply = SCM_CDR(reply);
	char *body = scm_to_locale_string(SCM_CAR(reply));
	send_all(sock, body); // body
	free(body);
//printf("SEND BODY\n");
	close(sock);
	scm_remember_upto_here_1(request);
	scm_remember_upto_here_1(reply);
	scm_remember_upto_here_1(headers);
	scm_remember_upto_here_1(cookie_header);
	return;
	}

static SCM dispatcher(void *data) {
	unsigned long id;
	RFRAME *frame;
	SCM res;
	time_t now;
	id = (unsigned long)scm_current_thread();
//printf("start thread %08lx\n", id);
	while (1) {
		scm_lock_mutex(qmutex);
		if (req_queue == NULL) {
//printf("wait queue %08lx\n", id);
			now = time(NULL) + 15;
			res = scm_timed_wait_condition_variable(qcondvar, qmutex,
					scm_from_double((double)now));
			if (res == SCM_BOOL_F) {
				scm_unlock_mutex(qmutex);
//printf("timed out %08lx\n", id);
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
//printf("dequeue request %08lx\n", id);
		process_request(frame);
		scm_lock_mutex(qmutex);
		busy_threads--;
		scm_unlock_mutex(qmutex);
		}
	return SCM_BOOL_T;
	}

static void add_thread() {
	SCM thread;
	if (nthreads >= MAX_THREADS) return;
	thread = scm_spawn_thread(dispatcher, NULL, NULL, NULL);
	threads = scm_cons(thread, threads);
	nthreads++;
printf("ADD THREAD: %d\n", nthreads);
	return;
	}

static SCM sigq() {
	scm_signal_condition_variable(qcondvar);
	return SCM_BOOL_T;
	}

static void init_env(void) {
	char *here, pats[64];
	int n;
	struct stat bstat;
	scm_c_define_gsubr("http", 2, 0, 0, set_handler);
	//scm_c_define_gsubr("not-found", 1, 0, 0, default_not_found);
	scm_c_define_gsubr("not-found", 1, 0, 0, dump_request);
	not_found = scm_c_eval_string("not-found");
	scm_c_define_gsubr("uuid-generate", 0, 0, 0, uuid_gen);
	scm_c_define_gsubr("err-handler", 1, 0, 1, err_handler);
	scm_c_define_gsubr("simple-response", 2, 0, 0, simple_http_response);
	scm_c_define_gsubr("json-response", 1, 0, 0, json_http_response);
	scm_c_define_gsubr("sigq", 0, 0, 0, sigq);
	scm_c_define("responders", threads = SCM_EOL);
	scm_c_define("qmutex", qmutex = scm_make_mutex());
	scm_c_define("pmutex", pmutex = scm_make_mutex());
	scm_c_define("qcondvar", qcondvar = scm_make_condition_variable());
	scm_c_define("threads", SCM_EOL);
	sprintf(pats, "%s=([0-9a-f]+)", COOKIE_KEY);
	regcomp(&cookie_pat, pats, REG_EXTENDED);
	init_postgres();
	init_time();
	init_cache();
	init_json();
	init_template();
	init_inotify();
	init_log();
	init_http();
	n = sysconf(_SC_NPROCESSORS_ONLN);
	busy_threads = 0;
	if (threading) while (n-- > 0) add_thread();
	here = getcwd(NULL, 0);
	if (chdir(gusher_root) == 0) {
		if (stat(BOOT_FILE, &bstat) == 0) {
			log_msg("load %s\n", BOOT_FILE);
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
	shutdown_inotify();
	shutdown_http();
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
			catch_proc, NULL,
			NULL, NULL);
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
			catch_proc, NULL,
			NULL, NULL);
	return;
	}

static void enqueue_frame(RFRAME *frame) {
	scm_lock_mutex(qmutex);
	frame->next = NULL;
	if (req_queue == NULL) req_queue = frame;
	else req_tail->next = frame;
	req_tail = frame;
	scm_unlock_mutex(qmutex);
	return;
	}

static void process_http(int sock) {
	socklen_t size;
	struct rframe *frame;
	struct sockaddr_in client;
	size = sizeof(struct sockaddr_in);
	frame = get_frame();
	frame->sock = accept(sock, (struct sockaddr *)&client, &size);
	strcpy(frame->ipaddr, inet_ntoa(client.sin_addr));
	frame->rport = ntohs(client.sin_port);
	frame->count = tcount;
	if (threading) {
//printf("DISPATCH %d\n", tcount);
		if (busy_threads >= nthreads) add_thread();
		enqueue_frame(frame);
		scm_signal_condition_variable(qcondvar);
//printf("RETURN %d\n", tcount);
		}
	else {
printf("RUN SYNC\n");
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

int main(int argc, char **argv) {
	fd_set fds;
	struct timeval timeout;
	int opt, sock;
	int hisock, fdin;
	int http_port;
	int background;
	http_port = DEFAULT_PORT;
	threading = 1;
	background = 0;
	gusher_root[0] = '\0';
	while ((opt = getopt(argc, argv, "sdp:")) != -1) {
		switch (opt) {
			case 'p':
				http_port = atoi(optarg);
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
		sprintf(gusher_root, "%s/gusher", LOCALLIB);
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
	hisock = fdin = fileno(stdin);
	if (sock > hisock) hisock = sock;
	if (inotify_fd > hisock) hisock = inotify_fd;
	running = 1;
	if (isatty(fdin))
		rl_callback_handler_install(prompt, line_handler);
	while (running) {  
		FD_ZERO(&fds);
		FD_SET(sock, &fds);
		FD_SET(inotify_fd, &fds);
		if (!background) FD_SET(fdin, &fds);
		timeout.tv_sec = 3;
		timeout.tv_usec = 0;
		select(hisock + 1, &fds, NULL, NULL, &timeout);
		if (running == 0) break; // why?
		if (FD_ISSET(fdin, &fds)) process_line(fdin);
		if (FD_ISSET(sock, &fds)) process_http(sock);
		if (FD_ISSET(inotify_fd, &fds)) process_inotify_event();
		}
	log_msg("bye!\n");
	close(sock);
	shutdown_env();
	return 0;
	} 
