#include <microhttpd.h>
#include <libguile.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define DEFAULT_PORT 8080
#define scm_sym(a) (scm_from_locale_symbol(a))

static int init_http_guile = 1;
static scm_t_bits mhd_conn_tag;
static SCM dispatch;

inline SCM string_pair(const char *key, const char *value) {
	return scm_cons(scm_sym(key), scm_from_locale_string(value));
	}

static int fill_table(void *cls, enum MHD_ValueKind kind,
			const char *key, const char *value) {
	SCM *alist;
	static char buf[2048];
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

static SCM reply_http(SCM conn_smob, SCM http_status, SCM headers, SCM content) {
	struct MHD_Connection *conn;
	struct MHD_Response *resp;
	SCM node, pair;
	int ret;
	char *ccontent;
	conn = (struct MHD_Connection *)SCM_SMOB_DATA(conn_smob);
	ccontent = scm_to_locale_string(content);
	resp = MHD_create_response_from_buffer(strlen(ccontent),
			(void *)ccontent, MHD_RESPMEM_MUST_COPY);
	node = headers;
	while (node != SCM_EOL) {
		pair = SCM_CAR(node);
		MHD_add_response_header(resp,
			scm_to_locale_string(SCM_CAR(pair)),
			scm_to_locale_string(SCM_CDR(pair))
			);
		node = SCM_CDR(node);
		}
	ret = MHD_queue_response(conn, scm_to_int(http_status), resp);
	MHD_destroy_response(resp);
	free(ccontent);
	return scm_from_signed_integer(ret);
	}

static int http_callback(void *cls, struct MHD_Connection *conn,
			const char *url, const char *method,
			const char *version, const char *upload_data,
			size_t *upload_data_size, void **con_cls) {
	SCM headers, query, cookies, conn_smob;
	int kind;
	if (init_http_guile) {
		init_http_guile = 0;
		scm_init_guile();
		mhd_conn_tag = scm_make_smob_type("mhd_conn", 
			sizeof(struct MHD_Connection *));
		scm_c_eval_string("(define http-handlers (make-hash-table))");
		scm_c_define_gsubr("reply-http", 4, 0, 0, reply_http);
		scm_c_primitive_load("boot.scm");
		dispatch = scm_c_eval_string("default-handler");
		}
	SCM_NEWSMOB(conn_smob, mhd_conn_tag, conn);
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
	scm_call_4(dispatch, conn_smob, headers, query, cookies);
	return MHD_YES;
	}

static SCM sayfoo(void) {
	printf("foo!\n");
	return SCM_UNSPECIFIED;
	}

static void guile_shell(void *closure, int argc, char **argv) {
	fprintf(stderr, "guile starting\n");
	scm_c_define_gsubr("foo", 0, 0, 0, sayfoo);
	scm_shell(argc, argv);
	}

int main(int argc, char **argv) {
	struct MHD_Daemon *daemon;
	int port;
	port = DEFAULT_PORT;
	daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY,
			port,
			NULL,
			NULL,
			&http_callback,
			NULL,
			MHD_OPTION_END);
	if (daemon == NULL) {
		fprintf(stderr, "can't start daemon, bye.\n");
		return 1;
		}
	//getchar();
	scm_boot_guile(argc, argv, guile_shell, 0);
	MHD_stop_daemon(daemon);
	return 0;
	}
