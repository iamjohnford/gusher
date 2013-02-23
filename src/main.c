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
#include <sys/socket.h>
#include <sys/select.h>
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

#include "postgres.h"
#include "gtime.h"
#include "redis.h"
#include "json.h"
#include "template.h"

#define makesym(s) (scm_from_locale_symbol(s))
#define addlist(list,item) (list=scm_cons((item),(list)))
#define DEFAULT_PORT 8080

struct handler_entry {
	char *path;
	SCM handler;
	struct handler_entry *link;
	};

static const char *prompt = "eval> ";
static int background = 0;
static int sock;
static int http_port = DEFAULT_PORT;
static struct handler_entry *handlers = NULL;

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
	scm_c_define("req-handlers", scm_acons(path, lambda,
		scm_c_eval_string("req-handlers")));
	entry = (struct handler_entry *)malloc(
				sizeof(struct handler_entry));
	entry->path = scm_to_locale_string(path);
printf("set handler for %s\n", entry->path);
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

static void parse_header(char *line, int reqline, SCM *env) {
	SCM value, query, qstring;
	char *mark, *pt;
	if (reqline) {
		if (line[0] == 'G') value = makesym("get");
		else value = makesym("post");
		addlist(*env, scm_cons(makesym("method"), value));
		mark = index(line, ' ') + 1;
		*(index(mark, ' ')) = '\0';
		addlist(*env, sym_string("url", mark));
		if ((pt = index(mark, '?')) != NULL) {
			*pt++ = '\0';
			qstring = scm_from_locale_string(pt);
			query = parse_query(pt);
			}
		else {
			query = SCM_EOL;
			qstring = scm_from_locale_string("");
			}
		addlist(*env, sym_string("url-path", mark));
		addlist(*env, scm_cons(makesym("query-string"), qstring));
		addlist(*env, scm_cons(makesym("query"), query));
		return;
		}
	mark = index(line, ':');
	*mark++ = '\0';
	while (isblank(*mark)) mark++;
	addlist(*env, sym_string(downcase(line), mark));
	}

static SCM default_not_found(SCM env) {
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

static SCM find_handler(SCM env) {
	char *spath;
	SCM path;
	struct handler_entry *pt;
	if ((path = scm_assq_ref(env, makesym("url-path"))) == SCM_BOOL_F)
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

static SCM dispatch(void *data) {
	socklen_t size;
	char *hname, *hvalue, *body, *status;
	int conn, reqline, eoh, n;
	SCM env, reply, handler, headers, pair;
	char *mark, *pt, buf[65536];
	struct sockaddr_in client;
	size = sizeof(struct sockaddr_in);
	//sock = *((int *)data);
	conn = accept(sock, (struct sockaddr *)&client, &size);
	env = SCM_EOL;
	addlist(env, sym_string("remote-host", inet_ntoa(client.sin_addr)));
	addlist(env, scm_cons(makesym("remote-port"),
		scm_from_signed_integer(ntohs(client.sin_port))));
	reqline = 1;
	eoh = 0;
	while (!eoh) {
		n = recv(conn, buf, sizeof(buf), 0);
		if (n == 0) {
			fprintf(stderr, "peer closed connection\n");
			return SCM_BOOL_F;
			}
		if (n < 0) {
			fprintf(stderr, "bad recv: %s\n", strerror(errno));
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
			parse_header(mark, reqline, &env);
			reqline = 0;
			mark = pt + 1;
			}
		}
	if ((handler = find_handler(env)) == SCM_BOOL_F) {
		handler = scm_c_eval_string("not-found");
		}
	reply = scm_call_1(handler, env);
	status = scm_buf_string(SCM_CAR(reply), buf, sizeof(buf));
	sprintf(buf, "HTTP/1.1 %s\r\n", status);
	send_all(conn, buf);
	reply = SCM_CDR(reply);
	headers = SCM_CAR(reply);
	while (headers != SCM_EOL) {
		pair = SCM_CAR(headers);
		hname = scm_buf_string(SCM_CAR(pair), buf, sizeof(buf));
		hvalue = scm_to_locale_string(SCM_CDR(pair));
		n = strlen(hvalue);
		hvalue = scm_buf_string(SCM_CDR(pair), &buf[n + 1],
				sizeof(buf) - n - 1);
		sprintf(buf, "%s: %s\r\n", hname, hvalue);
		send_all(conn, buf);
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

static SCM uuid_gen(void) {
	uuid_t out;
	char buf[33];
	int i;
	uuid_generate(out);
	for (i = 0; i < 16; i++) sprintf(&buf[i * 2], "%02x", out[i]);
	return scm_from_locale_string((const char *)buf);
	}

static void init_env(void) {
	scm_c_define_gsubr("set-handler", 2, 0, 0, set_handler);
	scm_c_define_gsubr("not-found", 1, 0, 0, default_not_found);
	scm_c_define_gsubr("uuid-generate", 0, 0, 0, uuid_gen);
	scm_c_define("req-handlers", SCM_EOL);
	init_postgres();
	init_time();
	init_redis();
	init_json();
	init_template();
	}

int main(int argc, char **argv) {
	fd_set fds;
	char buf[1024];
        int opt, n;
	int hisock, fdin;
	SCM display, newline, obj;
        struct sockaddr_in server_addr;    
        int optval;
	while ((opt = getopt(argc, argv, "dp:")) != -1) {
		switch (opt) {
			case 'p':
				http_port = atoi(optarg);
				break;
			case 'd':
				background = 1;
				break;
			default:
				fprintf(stderr, "invalid option: %c", opt);
				exit(1);
			}
		}
	sock = socket(AF_INET, SOCK_STREAM, 0);
	optval = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	memset(&server_addr, 0, sizeof(struct sockaddr_in));
        server_addr.sin_family = AF_INET;         
        server_addr.sin_port = htons(http_port);
        server_addr.sin_addr.s_addr = INADDR_ANY; 
        if (bind(sock, (struct sockaddr *)&server_addr,
			sizeof(struct sockaddr_in)) != 0) {
		fprintf(stderr, "can't bind: %s\n", strerror(errno));
		exit(1);
		};
        if (listen(sock, 5) != 0) {
		fprintf(stderr, "can't listen: %s\n", strerror(errno));
		exit(1);
		}
	scm_init_guile();
	init_env();
	hisock = fdin = fileno(stdin);
	if (sock > hisock) hisock = sock;
	fputs(prompt, stdout);
	fflush(stdout);
	display = scm_c_eval_string("display");
	newline = scm_c_eval_string("newline");
        while(1) {  
		FD_ZERO(&fds);
		FD_SET(sock, &fds);
		FD_SET(fdin, &fds);
		n = select(hisock + 1, &fds, NULL, NULL, NULL);
		if (FD_ISSET(fdin, &fds)) {
			n = read(fdin, buf, 1024);
			if (n == 0) break;
			if (n < 0) printf("err: %s\n", strerror(errno));
			buf[n] = '\0';
			while (isspace(buf[--n])) buf[n] = '\0';
printf("EVAL \"%s\"\n", buf);
fflush(stdout);
			obj = scm_c_eval_string(buf);
			if (obj != SCM_UNSPECIFIED) {
				scm_call_1(display, obj);
				scm_call_0(newline);
				}
			fputs(prompt, stdout);
			fflush(stdout);
			}
		if (FD_ISSET(sock, &fds)) {
			scm_spawn_thread(dispatch, (void *)&sock,
						NULL, NULL);
			}
		}
	fprintf(stderr, "bye!\n");
	close(sock);
	return 0;
	} 
