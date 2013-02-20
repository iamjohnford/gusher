#include <jansson.h>
#include <stdlib.h>
#include <libguile.h>
#include <stdio.h>

static char *make_key(SCM obj) {
	SCM string;
	if (scm_is_string(obj)) string = obj;
	else string = scm_symbol_to_string(obj);
	return scm_to_locale_string(string);
	}

static json_t *json_build(SCM obj) {
	json_t *jobj;
	SCM node, pair;
	char *buf;
	if (scm_is_string(obj)) {
		buf = scm_to_locale_string(obj);
		jobj = json_string(buf);
		free(buf);
		}
	else if (scm_is_symbol(obj)) {
		buf = scm_to_locale_string(scm_symbol_to_string(obj));
		jobj = json_string(buf);
		free(buf);
		}
	else if (scm_boolean_p(obj) == SCM_BOOL_T)
		jobj = (obj == SCM_BOOL_T ? json_true() : json_false());
	else if (scm_null_p(obj) == SCM_BOOL_T)
		jobj = json_null();
	else if (scm_integer_p(obj) == SCM_BOOL_T)
		jobj = json_integer(scm_to_int(obj));
	else if (scm_real_p(obj) == SCM_BOOL_T)
		jobj = json_real(scm_to_double(obj));
	else if ((scm_list_p(obj) == SCM_BOOL_T) &&
			(scm_pair_p(SCM_CAR(obj)) == SCM_BOOL_T)) {
		char *key;
		jobj = json_object();
		for (node = obj; node != SCM_EOL; node = SCM_CDR(node)) {
			pair = SCM_CAR(node);
			key = make_key(SCM_CAR(pair));
			json_object_set(jobj, key,
					json_build(SCM_CDR(pair)));
			free(key);
			}
		}
	else if (scm_list_p(obj) == SCM_BOOL_T) {
		jobj = json_array();
		for (node = obj; node != SCM_EOL; node = SCM_CDR(node))
			json_array_append(jobj, json_build(SCM_CAR(node)));
		}
	else jobj = json_null();
	return jobj;
	}

static SCM json_encode(SCM obj) {
	char *buf;
	SCM string;
	buf = json_dumps(json_build(obj), JSON_COMPACT);
	if (buf == NULL) {
		fprintf(stderr, "JSON encode failed\n");
		return SCM_BOOL_F;
		}
	string = scm_from_locale_string(buf);
	free(buf);
	return string;
	}

void init_json(void) {
	scm_c_define_gsubr("json-encode", 1, 0, 0, json_encode);
	}
