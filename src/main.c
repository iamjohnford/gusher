#include <microhttpd.h>
#include <libguile.h>
#include <errno.h>
#include <stdio.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include "postgres.h"
#include "gtime.h"
#include "redis.h"

#define DEFAULT_PORT 8080
#define scm_sym(a) (scm_from_locale_symbol(a))

struct mhd_conn {
	struct MHD_Connection *conn;
	};

struct handler_entry {
	char *path;
	SCM handler;
	struct handler_entry *link;
	};

static int init_http_guile = 1;
static scm_t_bits mhd_conn_tag;
static SCM not_found;
static struct handler_entry *handlers = NULL;

inline SCM string_pair(const char *key, const char *value) {
	return scm_cons(scm_sym(key), scm_from_locale_string(value));
	}

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
	entry = (struct handler_entry *)malloc(
				sizeof(struct handler_entry));
	entry->path = scm_to_locale_string(path);
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

/*static const char *query = "GET / HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";

static void prime_pump(int port) {
	int sock, n, len, sent;
	struct sockaddr_in *remote;
	char buf[1024];
	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	remote = (struct sockaddr_in *)malloc(sizeof(struct sockaddr_in));
	remote->sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1",
			(void *)(&(remote->sin_addr.s_addr)));
	remote->sin_port = htons(port);
	connect(sock, (struct sockaddr *)remote, sizeof(struct sockaddr));
	sent = 0;
	len = strlen(query);
	while (sent < len) {
		n = send(sock, query + sent, len - sent, 0);
		sent += n;
		}
	n = recv(sock, buf, 1024, 0);
	buf[n] = '\0';
//fprintf(stderr, "REPLY: %s\n", buf);
	close(sock);
	free(remote);
	return;
	}
*/

static int fill_table(void *cls, enum MHD_ValueKind kind,
			const char *key, const char *value) {
	SCM *alist;
	char buf[2048];
	int i, n;
	alist = (SCM *)cls;
	if (value == NULL) value = "";
	n = strlen(key);
	for (i = 0; i < n; i++) {
		if (i >= sizeof(buf)) {
			i = sizeof(buf) - 1;
			break;
			}
		buf[i] = tolower(key[i]);
		}
	buf[i] = '\0';
	*alist = scm_cons(string_pair(buf, value), *alist);
	return MHD_YES;
	}	

static SCM reply_http(SCM conn_smob, SCM args) {
	struct MHD_Response *resp;
	struct mhd_conn *conn;
	SCM node, pair, headers;
	int ret, n;
	int http_status;
	char *content;
	char hname[256];
	char hval[256];
	http_status = scm_to_int(SCM_CAR(args));
	args = SCM_CDR(args);
	headers = SCM_CAR(args);
	args = SCM_CDR(args);
	content = scm_to_locale_string(SCM_CAR(args));
	conn = (struct mhd_conn *)SCM_SMOB_DATA(conn_smob);
	resp = MHD_create_response_from_buffer(strlen(content),
			(void *)content, MHD_RESPMEM_MUST_COPY);
	node = headers;
	while (node != SCM_EOL) {
		pair = SCM_CAR(node);
		n = scm_to_locale_stringbuf(SCM_CAR(pair), hname, 256);
		hname[n] = '\0';
		n = scm_to_locale_stringbuf(SCM_CDR(pair), hval, 256);
		hval[n] = '\0';
		MHD_add_response_header(resp, hname, hval);
		node = SCM_CDR(node);
		}
	ret = MHD_queue_response(conn->conn, http_status, resp);
	MHD_destroy_response(resp);
	free(content);
	return scm_from_signed_integer(ret);
	}

static SCM intern_not_found(SCM conn) {
	SCM args, headers;
	args = SCM_EOL;
	args = scm_cons(scm_from_locale_string("Not Found"), args);
	headers = SCM_EOL;
	headers = scm_cons(scm_cons(scm_from_locale_string("content-type"),
				scm_from_locale_string("text/plain")),
				headers);
	args = scm_cons(headers, args);
	args = scm_cons(scm_from_signed_integer(404), args);
	return reply_http(conn, args);
	}

static SCM find_handler(const char *url) {
	struct handler_entry *pt;
	for (pt = handlers; pt != NULL; pt = pt->link) {
		if (strncmp(pt->path, url, strlen(pt->path)) == 0)
			return pt->handler;
		}
	return SCM_BOOL_F;
	}

static int http_callback(void *cls, struct MHD_Connection *conn,
			const char *url, const char *method,
			const char *version, const char *upload_data,
			size_t *upload_data_size, void **con_cls) {
	SCM headers, query, cookies, conn_smob;
	SCM handler;
	int kind;
	struct mhd_conn *mconn;
	if (init_http_guile) {
		init_http_guile = 0;
		scm_init_guile();
		mhd_conn_tag = scm_make_smob_type("mhd_conn", 
			sizeof(struct MHD_Connection *));
		scm_c_define_gsubr("set-handler", 2, 0, 0, set_handler);
		scm_c_define_gsubr("reply-http", 2, 0, 0, reply_http);
		scm_c_define_gsubr("not-found", 1, 0, 0, intern_not_found);
		scm_c_define("req-handlers", SCM_EOL);
		init_postgres();
		init_time();
		init_redis();
		scm_c_primitive_load("boot.scm");
		not_found = scm_c_eval_string("not-found");
		}
	mconn = (struct mhd_conn *)scm_gc_malloc(sizeof(struct mhd_conn),
					"mhd_conn");
	mconn->conn = conn;
	SCM_NEWSMOB(conn_smob, mhd_conn_tag, mconn);
	if ((handler = find_handler(url)) == SCM_BOOL_F) {
		scm_call_1(not_found, conn_smob);
		return MHD_YES;
		}
	headers = SCM_EOL;
	MHD_get_connection_values(conn, MHD_HEADER_KIND, fill_table,
				(void *)&headers);
	headers = scm_cons(string_pair("method", method), headers);
	headers = scm_cons(string_pair("uri", url), headers);
	headers = scm_cons(string_pair("version", version), headers);
	query = SCM_EOL;
	if (strcmp(method, "POST") == 0) kind = MHD_POSTDATA_KIND;
	else kind = MHD_GET_ARGUMENT_KIND;
	MHD_get_connection_values(conn, kind, fill_table, (void *)&query);
	cookies = SCM_EOL;
	MHD_get_connection_values(conn, MHD_COOKIE_KIND, fill_table,
				(void *)&cookies);
	reply_http(conn_smob, scm_call_3(handler, headers, query, cookies));
	return MHD_YES;
	}

static void guile_shell(void *closure, int argc, char **argv) {
	fprintf(stderr, "guile starting\n");
	init_postgres();
	init_time();
	init_redis();
	//prime_pump(8080);
fprintf(stderr, "start shell\n");
	scm_shell(argc, argv);
	}

int main(int argc, char **argv) {
	struct MHD_Daemon *daemon;
	int http_port, opt;
	int background;
	http_port = DEFAULT_PORT;
	background = 0;
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
	if (background) {
		signal(SIGCHLD, SIG_IGN);
		if (fork() > 0) _exit(0);
		}
	daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY,
			http_port,
			NULL,
			NULL,
			&http_callback,
			NULL,
			MHD_OPTION_END);
	if (daemon == NULL) {
		fprintf(stderr, "can't start daemon: %s.\n", strerror(errno));
		return 1;
		}
	if (background) {
		while (1) sleep(1);
		}
	else {
		//prime_pump(8080);
		scm_boot_guile(argc, argv, guile_shell, 0);
		}
	MHD_stop_daemon(daemon);
	return 0;
	}
