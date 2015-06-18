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

#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <libguile.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <db.h>

#include "log.h"
#include "json.h"

#define DEFAULT_REDIS_PORT 6379
#define FILE_CACHE "file-cache"
#define TYPE_DATUM 0
#define TYPE_FILE 1
#define TYPE_CHAIN 2
#define KC_ROOT "/var/lib/gusher/kv"

typedef struct make_node {
	SCM callback;
	SCM payload;
	SCM ascendants;
	SCM mutex;
	char *filepath;
	int type;
	int dirty;
	} MAKE_NODE;

typedef struct kvdb_node {
	DB *db;
	char *path;
	} KVDB_NODE;

typedef struct file_node {
	MAKE_NODE *node;
	time_t mtime;
	struct file_node *next;
	} FILE_NODE;

static SCM file_sym;
static SCM data_sym;
static SCM stamp_sym;
static FILE_NODE *file_nodes = NULL;
static scm_t_bits make_node_tag;
static scm_t_bits kvdb_node_tag;
static int check_kv_root = 1;
static SCM sessions_db;
extern SCM session_sym;

static void invalidate(MAKE_NODE *node) {
	SCM cursor;
	node->dirty = 1;
	cursor = node->ascendants;
	while (cursor != SCM_EOL) {
		invalidate((MAKE_NODE *)SCM_SMOB_DATA(SCM_CAR(cursor)));
		cursor = SCM_CDR(cursor);
		}
	scm_remember_upto_here_1(cursor);
	return;
	}

static SCM touched_node(SCM doc) {
	MAKE_NODE *node;
	node = (MAKE_NODE *)SCM_SMOB_DATA(doc);
	return (node->dirty ? SCM_BOOL_T : SCM_BOOL_F);
	}

static SCM touch_node(SCM doc, SCM args) {
	MAKE_NODE *node;
	node = (MAKE_NODE *)SCM_SMOB_DATA(doc);
	scm_lock_mutex(node->mutex);
	invalidate(node);
	if (scm_is_null(args)) {
		scm_unlock_mutex(node->mutex);
		return SCM_BOOL_T;
		}
	switch (node->type) {
	case TYPE_DATUM:
		node->payload = SCM_CAR(args);
		break;
	case TYPE_FILE:
		free(node->filepath);
		node->filepath = scm_to_locale_string(SCM_CAR(args));
		break;
		}
	scm_unlock_mutex(node->mutex);
	scm_remember_upto_here_2(doc, args);
	return SCM_BOOL_T;
	}

static char *load_from_file(const char *path) {
	char *buf;
	int fd, n;
	struct stat bstat;
	if (stat(path, &bstat) != 0) {
		perror("load-from-file[1]");
		return NULL;
		}
	if ((fd = open(path, O_RDONLY)) < 0) {
		perror("load-from-file[2]");
		return NULL;
		}
	buf = (char *)malloc(bstat.st_size + 1);
	n = read(fd, (void *)buf, bstat.st_size);
	buf[n] = '\0';
	close(fd);
	return buf;
	}

static SCM fetch_node(SCM smob, SCM args) {
	MAKE_NODE * node;
	SCM payload;
	char *buf;
	node = (MAKE_NODE *)SCM_SMOB_DATA(smob);
	scm_lock_mutex(node->mutex);
	if (!node->dirty) {
		payload = node->payload;
		scm_unlock_mutex(node->mutex);
		return payload;
		}
	//log_msg("REGENERATE %08x\n", (unsigned long)smob);
	node->dirty = 0;
	switch (node->type) {
	case TYPE_DATUM: break;
	case TYPE_FILE:
		buf = load_from_file(node->filepath);
		if (buf != NULL)
			node->payload = scm_take_locale_string(buf);
		else node->payload = SCM_BOOL_F;
		break;
	case TYPE_CHAIN:
		node->payload = scm_apply_0(node->callback, args);
		break;
		}
	payload = node->payload;
	scm_unlock_mutex(node->mutex);
	scm_remember_upto_here_2(smob, args);
	scm_remember_upto_here_1(payload);
	return payload;
	}

static MAKE_NODE *make_node(int type) {
	MAKE_NODE *node;
	node = (MAKE_NODE *)scm_gc_malloc(sizeof(MAKE_NODE), "make-node");
	node->filepath = NULL;
	node->callback = SCM_BOOL_F;
	node->payload = SCM_BOOL_F;
	node->ascendants = SCM_EOL;
	node->mutex = scm_make_mutex();
	node->type = type;
	return node;
	}

static void add_ascendant(SCM dependent, SCM self) {
	MAKE_NODE *node;
	SCM list;
	node = (MAKE_NODE *)SCM_SMOB_DATA(dependent);
	scm_lock_mutex(node->mutex);
	list = node->ascendants;
	while (list != SCM_EOL) {
		if (scm_is_eq(SCM_CAR(list), self)) {
			scm_unlock_mutex(node->mutex);
			return;
			}
		list = SCM_CDR(list);
		}
	node->ascendants = scm_cons(self, node->ascendants);
	scm_unlock_mutex(node->mutex);
	return;
	}

static SCM make_doc(SCM ingredients, SCM recipe) {
	MAKE_NODE *node;
	FILE_NODE *fnode;
	SCM smob, cursor;
	if (scm_is_symbol(ingredients)) {
		if (ingredients == file_sym) {
			node = make_node(TYPE_FILE);
			node->filepath = scm_to_locale_string(recipe);
			node->dirty = 1;
			fnode = (FILE_NODE *)malloc(sizeof(FILE_NODE));
			fnode->node = node;
			fnode->mtime = 0;
			fnode->next = file_nodes;
			file_nodes = fnode;
			}
		else {
			node = make_node(TYPE_DATUM);
			node->dirty = 0;
			node->payload = recipe;
			}
		SCM_RETURN_NEWSMOB(make_node_tag, node);
		}
	node = make_node(TYPE_CHAIN);
	node->dirty = 1;
	node->callback = recipe;
	SCM_NEWSMOB(smob, make_node_tag, node);
	cursor = ingredients;
	while (cursor != SCM_EOL) {
		add_ascendant(SCM_CAR(cursor), smob);
		cursor = SCM_CDR(cursor);
		}
	scm_remember_upto_here_2(ingredients, recipe);
	scm_remember_upto_here_2(smob, cursor);
	return smob;
	}

static size_t free_node(SCM smob) {
	MAKE_NODE *node;
	node = (MAKE_NODE *)SCM_SMOB_DATA(smob);
	free(node->filepath);
	return 0;
	}

static SCM mark_node(SCM smob) {
	MAKE_NODE *node;
	node = (MAKE_NODE *)SCM_SMOB_DATA(smob);
	scm_gc_mark(node->callback);
	scm_gc_mark(node->payload);
	scm_gc_mark(node->ascendants);
	scm_gc_mark(node->mutex);
	return SCM_UNSPECIFIED;
	}

inline char *symstr(SCM key) {
	if (scm_is_symbol(key))
		return scm_to_locale_string(scm_symbol_to_string(key));
	return scm_to_locale_string(key);
	}

static SCM kv_set(SCM db, SCM key, SCM value) {
	int err;
	char *skey, *sval;
	DBT dbkey, dbval;
	KVDB_NODE *node;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	skey = symstr(key);
	sval = scm_to_locale_string(value);
	memset(&dbkey, 0, sizeof(DBT));
	dbkey.data = skey;
	dbkey.size = strlen(skey);
	memset(&dbval, 0, sizeof(DBT));
	dbval.data = sval;
	dbval.size = strlen(sval);
	err = node->db->put(node->db, NULL, &dbkey, &dbval, 0);
	free(skey);
	free(sval);
	return (err == 0 ? SCM_BOOL_T : SCM_BOOL_F);
	}

static SCM kv_get(SCM db, SCM key) {
	int err;
	char *skey;
	DBT dbkey, dbval;
	KVDB_NODE *node;
	SCM out;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	skey = symstr(key);
	memset(&dbkey, 0, sizeof(DBT));
	dbkey.data = skey;
	dbkey.size = strlen(skey);
	memset(&dbval, 0, sizeof(DBT));
	dbval.flags = DB_DBT_REALLOC;
	err = node->db->get(node->db, NULL, &dbkey, &dbval, 0);
	if (err == 0) out = scm_from_locale_stringn(dbval.data, dbval.size);
	else if (err == DB_NOTFOUND) out = SCM_BOOL_F;
	else {
		out = SCM_BOOL_F;
		log_msg("kv-get '%s': %d\n", skey, err);
		}
	free(dbval.data);
	free(skey);
	return out;
	}

static SCM kv_exists(SCM db, SCM key) {
	int err;
	char *skey;
	DBT dbkey, dbval;
	KVDB_NODE *node;
	SCM out;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	skey = symstr(key);
	memset(&dbkey, 0, sizeof(DBT));
	dbkey.data = skey;
	dbkey.size = strlen(skey);
	memset(&dbval, 0, sizeof(DBT));
	dbval.flags = DB_DBT_REALLOC;
	err = node->db->get(node->db, NULL, &dbkey, &dbval, 0);
	if (err == 0) out = SCM_BOOL_T;
	else if (err == DB_NOTFOUND) out = SCM_BOOL_F;
	else {
		out = SCM_BOOL_F;
		log_msg("kv-exists '%s': %d\n", skey, err);
		}
	free(dbval.data);
	free(skey);
	return out;
	}

static SCM kv_count(SCM db) {
	KVDB_NODE *node;
	DB_HASH_STAT *stat;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	node->db->stat(node->db, NULL, &stat, 0);
	return scm_from_unsigned_integer((unsigned int)stat->hash_ndata);
	}

static SCM kv_keys(SCM db) {
	KVDB_NODE *node;
	int err;
	DBC *cursor;
	DBT dbkey, dbval;
	SCM list;
	list = SCM_EOL;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	node->db->cursor(node->db, NULL, &cursor, 0);
	memset(&dbkey, 0, sizeof(DBT));
	memset(&dbval, 0, sizeof(DBT));
	err = cursor->c_get(cursor, &dbkey, &dbval, DB_FIRST);
	while (err == 0) {
		list = scm_cons(scm_from_locale_stringn(dbkey.data, dbkey.size), list);
		err = cursor->c_get(cursor, &dbkey, &dbval, DB_NEXT);
		}
	cursor->c_close(cursor);
	scm_remember_upto_here_2(list, db);
	return list;
	}

static SCM kv_del(SCM db, SCM key) {
	KVDB_NODE *node;
	int err;
	char *skey;
	DBT dbkey;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	skey = symstr(key);
	memset(&dbkey, 0, sizeof(DBT));
	dbkey.data = skey;
	dbkey.size = strlen(skey);
	err = node->db->del(node->db, NULL, &dbkey, 0);
	free(skey);
	return (err == 0 ? SCM_BOOL_T : SCM_BOOL_F);
	}

void police_cache(void) {
	FILE_NODE *node;
	struct stat nstat;
	for (node = file_nodes; node != NULL; node = node->next) {
		if (stat(node->node->filepath, &nstat) != 0) continue;
		if (node->mtime == 0) {
			node->mtime = nstat.st_mtime;
			continue;
			}
		if (nstat.st_mtime <= node->mtime) continue;
		node->mtime = nstat.st_mtime;
		invalidate(node->node);
		}
	return;
	}

static SCM kv_open(SCM entry, SCM readonly) {
	static char path[PATH_MAX];
	int err;
	KVDB_NODE *node;
	DB *db;
	if (check_kv_root) {
		check_kv_root = 0;
		struct stat sts;
		if (stat(KC_ROOT, &sts) != 0) {
			log_msg("please provision '%s' for key-value storage\n", KC_ROOT);
			return SCM_BOOL_F;
			}
		}
	err = db_create(&db, NULL, 0);
	if (err != 0) {
		log_msg("db_create: %d\n", err);
		return SCM_BOOL_F;
		}
	char *sentry = scm_to_locale_string(entry);
	snprintf(path, sizeof(path) - 1, "%s/%s.db", KC_ROOT, sentry);
	path[sizeof(path) - 1] = '\0';
	free(sentry);
	int flags = DB_THREAD;
	if (readonly == SCM_BOOL_T) flags |= DB_RDONLY;
	else flags |= DB_CREATE;
	err = db->open(db, NULL, path, NULL, DB_HASH, flags, 0);
	if (err != 0) {
		log_msg("db_open '%s': %d,  %s\n", path, err, strerror(errno));
		db->close(db, 0);
		return SCM_BOOL_F;
		}
	node = (KVDB_NODE *)scm_gc_malloc(sizeof(KVDB_NODE), "kvdb-node");
	node->db = db;
	node->path = (char *)malloc(strlen(path) + 1);
	strcpy(node->path, path);
	chmod(path, 0664);
	SCM_RETURN_NEWSMOB(kvdb_node_tag, node);
	}

static SCM kv_close(SCM kvdb) {
	KVDB_NODE *node;
	node = (KVDB_NODE *)SCM_SMOB_DATA(kvdb);
	if (node->db != NULL) {
		node->db->close(node->db, 0);
		node->db = NULL;
		}
	if (node->path != NULL) {
		free(node->path);
		node->path = NULL;
		}
	return SCM_BOOL_T;
	}

static size_t free_kvdb(SCM smob) {
	kv_close(smob);
	return 0;
	}

static SCM session_key(SCM request) {
	return scm_assq_ref(request, session_sym);
	}

static SCM session_set(SCM request, SCM key, SCM value) {
	SCM db = kv_open(sessions_db, SCM_BOOL_F);
	SCM sesskey = session_key(request);
	SCM alist = kv_get(db, sesskey);
	if (alist == SCM_BOOL_F) alist = SCM_EOL;
	else alist = json_decode(alist);
	alist = scm_assq_set_x(alist, stamp_sym, scm_from_ulong(time(NULL)));
	alist = scm_assq_set_x(alist, key, value);
	SCM res = kv_set(db, sesskey, json_encode(alist));
	kv_close(db);
	scm_remember_upto_here_2(db, sesskey);
	scm_remember_upto_here_2(alist, res);
	return res;
	}

static SCM session_read(SCM request) {
	SCM val;
	SCM db = kv_open(sessions_db, SCM_BOOL_F);
	SCM sesskey = session_key(request);
	val = kv_get(db, sesskey);
	scm_remember_upto_here_1(sesskey);
	kv_close(db);
	if (val == SCM_BOOL_F) return SCM_BOOL_F;
	scm_remember_upto_here_2(val, db);
	return json_decode(val);
	}

static SCM session_get(SCM request, SCM key) {
	SCM sess_data = session_read(request);
	if (sess_data == SCM_BOOL_F) return SCM_BOOL_F;
	scm_remember_upto_here_1(sess_data);
	return scm_assq_ref(sess_data, key);
	}

void shutdown_cache(void) {
	FILE_NODE *next;
	while (file_nodes != NULL) {
		next = file_nodes->next;
		free(file_nodes);
		file_nodes = next;
		}
	return;
	}

void init_cache(void) {
	make_node_tag = scm_make_smob_type("make-node", sizeof(MAKE_NODE));
	scm_set_smob_free(make_node_tag, free_node);
	scm_set_smob_mark(make_node_tag, mark_node);
	kvdb_node_tag = scm_make_smob_type("kvdb-node", sizeof(KVDB_NODE));
	scm_set_smob_free(kvdb_node_tag, free_kvdb);
	sessions_db = scm_from_locale_string("sessions");
	scm_gc_protect_object(sessions_db);
	scm_permanent_object(file_sym = scm_from_utf8_symbol("file"));
	scm_permanent_object(data_sym = scm_from_utf8_symbol("data"));
	scm_permanent_object(stamp_sym = scm_from_utf8_symbol("stamp"));
	scm_c_define_gsubr("make-doc", 2, 0, 0, make_doc);
	scm_c_define_gsubr("touch-doc", 1, 0, 1, touch_node);
	scm_c_define_gsubr("fetch-doc", 1, 0, 1, fetch_node);
	scm_c_define_gsubr("touched-doc?", 1, 0, 0, touched_node);
	scm_c_define_gsubr("kv-open", 2, 0, 0, kv_open);
	scm_c_define_gsubr("kv-close", 1, 0, 0, kv_close);
	scm_c_define_gsubr("kv-set", 3, 0, 0, kv_set);
	scm_c_define_gsubr("kv-get", 2, 0, 0, kv_get);
	scm_c_define_gsubr("kv-exists", 2, 0, 0, kv_exists);
	scm_c_define_gsubr("kv-count", 1, 0, 0, kv_count);
	scm_c_define_gsubr("kv-keys", 1, 0, 0, kv_keys);
	scm_c_define_gsubr("kv-del", 2, 0, 0, kv_del);
	scm_c_define_gsubr("session-key", 1, 0, 0, session_key);
	scm_c_define_gsubr("session-read", 1, 0, 0, session_read);
	scm_c_define_gsubr("session-get", 2, 0, 0, session_get);
	scm_c_define_gsubr("session-set", 3, 0, 0, session_set);
	}
