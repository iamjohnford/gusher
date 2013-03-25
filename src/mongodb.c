
#define MONGO_HAVE_UNISTD

#include <mongo.h>
#include <libguile.h>
#include <unistd.h>


static mongo conn;
static int mongo_status;

static SCM mongo_store(SCM namespace, SCM doc) {
	bson_oid_t oid;
	bson b;
	int stat;
	char *ns, *field, *buf;
	char oidbuf[64];
	SCM pair, node, value;
	if (mongo_status != MONGO_OK) return SCM_BOOL_F;
	ns = scm_to_locale_string(namespace);
	bson_init(&b);
	bson_oid_gen(&oid);
	bson_oid_to_string(&oid, oidbuf);
	bson_append_oid(&b, "_id", &oid);
	node = doc;
	while (node != SCM_EOL) {
		pair = SCM_CAR(node);
		field = scm_to_locale_string(scm_symbol_to_string(SCM_CAR(pair)));
		buf = NULL;
		value = SCM_CDR(pair);
		if (scm_is_string(value)) {
			buf = scm_to_locale_string(value);
			stat = bson_append_string(&b, field, buf);
			}
		else if (scm_is_symbol(value)) {
			buf = scm_to_locale_string(scm_symbol_to_string(value));
			bson_append_string(&b, field, buf);
			}
		else if (value == SCM_BOOL_T) {
			bson_append_bool(&b, field, 1);
			}
		else if (value == SCM_BOOL_F) {
			bson_append_bool(&b, field, 0);
			}
		else if (scm_is_null(value)) {
			bson_append_null(&b, field);
			}
		else if (scm_is_integer(value)) {
			bson_append_int(&b, field, scm_to_int(value));
			}
		else if (scm_is_real(value)) {
			bson_append_double(&b, field, scm_to_double(value));
			}
		free(field);
		if (buf != NULL) free(buf);
		node = SCM_CDR(node);
		}
	bson_finish(&b);
	mongo_insert(&conn, ns, &b, NULL);
	free(ns);
	bson_destroy(&b);
	return scm_from_locale_string(oidbuf);
	}

void init_mongodb() {
	mongo_init(&conn);
	mongo_status = mongo_client(&conn, "127.0.0.1", 27017);
	if (mongo_status != MONGO_OK) {
		fprintf(stderr, "mongo err: %d\n", mongo_status);
		}
	scm_c_define_gsubr("store", 2, 0, 0, mongo_store);
	}

void shutdown_mongodb() {
	if (mongo_status == MONGO_OK) mongo_destroy(&conn);
	}
