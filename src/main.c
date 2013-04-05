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

struct handler_entry {
	char *path;
	SCM handler;
	struct handler_entry *link;
	};

static int running;
static const char *prompt = "gusher> ";
static struct handler_entry *handlers = NULL;
static char gusher_root[1024];
static regex_t cookie_pat;
static const char *hex = "0123456789abcdef";

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
		addlist(*request, scm_cons(makesym("query-string"),
				qstring));
		addlist(*request, scm_cons(makesym("query"), query));
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
	return resp;
	}

static SCM find_handler(SCM request) {
	char *spath;
	SCM path;
	struct handler_entry *pt;
	if ((path = scm_assq_ref(request, makesym("url-path")))
					== SCM_BOOL_F)
		return SCM_BOOL_F;
	spath = scm_to_locale_string(path);
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
	return resp;
	}

static SCM json_http_response(SCM enc_content) {
	SCM headers, resp;
	char *buf, clen[16];
	buf = scm_to_locale_string(enc_content);
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

static SCM dispatch(void *data) {
	socklen_t size;
	int sock;
	char *hname, *hvalue, *body, *status, *cookie;
	int conn, reqline, eoh, n;
	SCM request, reply, handler, headers, pair, val;
	char *mark, *pt, buf[65536], sbuf[256];
	struct sockaddr_in client;
	size = sizeof(struct sockaddr_in);
	sock = *((int *)data);
	conn = accept(sock, (struct sockaddr *)&client, &size);
	request = SCM_EOL;
	addlist(request, sym_string("remote-host",
				inet_ntoa(client.sin_addr)));
	addlist(request, scm_cons(makesym("remote-port"),
		scm_from_signed_integer(ntohs(client.sin_port))));
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
	if ((handler = find_handler(request)) == SCM_BOOL_F) {
		handler = scm_c_eval_string("not-found");
		}
	reply = scm_call_1(handler, request);
	status = scm_buf_string(SCM_CAR(reply), buf, sizeof(buf));
	sprintf(sbuf, "HTTP/1.1 %s\r\n", status);
	send_all(conn, sbuf);
	reply = SCM_CDR(reply);
	headers = SCM_CAR(reply);
	if ((cookie = session_cookie(request)) == NULL) {
		char buf[128];
		cookie = (char *)malloc(33);
		put_uuid(cookie);
		sprintf(buf, "%s=%s; Path=/", COOKIE_KEY, cookie);
		headers = scm_acons(
			scm_from_locale_string("set-cookie"),
			scm_from_locale_string(buf),
			headers);
		}
	if (get_session(cookie) == SCM_BOOL_F) {
		SCM session;
		session = SCM_EOL;
		session = scm_acons(makesym("new"), SCM_BOOL_T, session);
		put_session(cookie, session);
		}
	request = scm_acons(makesym("session"),
					scm_take_locale_string(cookie), request);
scm_call_1(scm_c_eval_string("display"), request);
scm_call_0(scm_c_eval_string("newline"));
	while (headers != SCM_EOL) {
		pair = SCM_CAR(headers);
		hname = scm_buf_string(SCM_CAR(pair), buf, sizeof(buf));
		send_all(conn, hname);
		send_all(conn, ": ");
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
		headers = SCM_CDR(headers);
		}
	send_all(conn, "\r\n");
	reply = SCM_CDR(reply);
	body = scm_to_locale_string(SCM_CAR(reply));
	send_all(conn, body);
	free(body);
	close(conn);
	return SCM_BOOL_T;
	}

static SCM err_handler(SCM key, SCM rest) {
	SCM display, newline;
	display = scm_c_eval_string("display");
	newline = scm_c_eval_string("newline");
	scm_call_1(display, rest);
	scm_call_0(newline);
	return SCM_UNSPECIFIED;
	}

static void init_env(void) {
	char *here, pats[64];
	struct stat bstat;
	scm_c_define_gsubr("http", 2, 0, 0, set_handler);
	scm_c_define_gsubr("not-found", 1, 0, 0, default_not_found);
	scm_c_define_gsubr("uuid-generate", 0, 0, 0, uuid_gen);
	scm_c_define_gsubr("err-handler", 1, 0, 1, err_handler);
	scm_c_define_gsubr("simple-response", 2, 0, 0, simple_http_response);
	scm_c_define_gsubr("json-response", 1, 0, 0, json_http_response);
	scm_c_define("responders", SCM_EOL);
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

static void shutdown_env(void) {
	regfree(&cookie_pat);
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

static int threading;

static void process_http(int sock) {
	int sockc;
	sockc = sock;
	if (threading) {
		scm_spawn_thread(dispatch, (void *)&sockc,
				NULL, NULL);
		}
	else dispatch((void *)&sockc);
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
	while ((opt = getopt(argc, argv, "mdp:")) != -1) {
		switch (opt) {
			case 'p':
				http_port = atoi(optarg);
				break;
			case 'd':
				background = 1;
				break;
			case 'm':
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
