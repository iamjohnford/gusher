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
#include <kclangc.h>

#include "gnotify.h"
#include "log.h"
#include "json.h"

#define DEFAULT_REDIS_PORT 6379
#define FILE_CACHE "file-cache"
#define TYPE_DATUM 0
#define TYPE_FILE 1
#define TYPE_CHAIN 2
#define KC_ROOT "/var/lib/gusher/kc"

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
	KCDB *db;
	char *path;
	} KVDB_NODE;

typedef struct file_node {
	MAKE_NODE *node;
	time_t mtime;
	struct file_node *next;
	} FILE_NODE;

static SCM file_sym;
static SCM data_sym;
static FILE_NODE *file_nodes = NULL;
static scm_t_bits make_node_tag;
static scm_t_bits kvdb_node_tag;
static SCM sessions_db;

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
	log_msg("REGENERATE %08x\n", (unsigned long)smob);
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
		if (scm_is_null(args))
			node->payload = scm_call_0(node->callback);
		else
			node->payload = scm_call_1(node->callback,
						SCM_CAR(args));
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
	scm_remember_upto_here_1(smob);
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
	int res;
	char *skey, *sval;
	KVDB_NODE *node;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	skey = symstr(key);
	sval = scm_to_locale_string(value);
	res = kcdbset(node->db, skey, strlen(skey), sval, strlen(sval));
	free(skey);
	free(sval);
	return (res == 0 ? SCM_BOOL_F : SCM_BOOL_T);
	}

SCM put_session(const char *sesskey, SCM table) {
	return kv_set(sessions_db, scm_from_locale_string(sesskey),
						json_encode(table));
	}

static SCM kv_get(SCM db, SCM key) {
	size_t vsiz;
	char *skey, *value;
	KVDB_NODE *node;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	skey = symstr(key);
	vsiz = kcdbcheck(node->db, skey, strlen(skey));
	if (vsiz < 0) {
		free(skey);
		return SCM_BOOL_F;
		}
	value = (char *)malloc(vsiz + 1);
	kcdbgetbuf(node->db, skey, strlen(skey), value, vsiz);
	value[vsiz] = '\0';
	free(skey);
	return scm_take_locale_string(value);
	}

static SCM kv_exists(SCM db, SCM key) {
	size_t vsiz;
	char *skey;
	KVDB_NODE *node;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	skey = symstr(key);
	vsiz = kcdbcheck(node->db, skey, strlen(skey));
	free(skey);
	return (vsiz >= 0 ? SCM_BOOL_T : SCM_BOOL_F);
	}

static SCM kv_count(SCM db) {
	KVDB_NODE *node;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	return scm_from_unsigned_integer(kcdbcount(node->db));
	}

static SCM kv_keys(SCM db) {
	KVDB_NODE *node;
	size_t n;
	char **keys;
	SCM list;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	n = kcdbcount(node->db);
	keys = (char **)malloc(sizeof(char *) * n);
	n = kcdbmatchprefix(node->db, "", keys, n);
	if (n < 0) {
		free(keys);
		return SCM_BOOL_F;
		}
	list = SCM_EOL;
	while(n > 0) {
		n--;
		list = scm_cons(scm_from_locale_string(keys[n]), list);
		kcfree(keys[n]);
		}
	free(keys);
	scm_remember_upto_here_1(list);
	return list;
	}

static SCM kv_del(SCM db, SCM key) {
	KVDB_NODE *node;
	int res;
	char *skey;
	node = (KVDB_NODE *)SCM_SMOB_DATA(db);
	skey = symstr(key);
	res = kcdbremove(node->db, skey, strlen(skey));
	free(skey);
	return (res ? SCM_BOOL_T : SCM_BOOL_F);
	}

SCM get_session(const char *sesskey) {
	SCM val;
	val = kv_get(sessions_db, scm_from_locale_string(sesskey));
	if (val == SCM_BOOL_F) return SCM_BOOL_F;
	scm_remember_upto_here_1(val);
	return json_decode(val);
	}

/*
static SCM edit_watch_handler(SCM path, SCM mask) {
	char *spath;
	spath = scm_to_locale_string(path);
	scm_lock_mutex(redis_mutex);
	send_header("HDEL", 2);
	send_arg(FILE_CACHE);
	send_arg(spath);
	getrline(NULL);
	free(spath);
	scm_unlock_mutex(redis_mutex);
	return SCM_BOOL_T;
	}

static SCM watch_edit(SCM dir) {
	if (redis_sock < 0) return SCM_BOOL_F;
	return add_watch(dir, scm_from_uint32(IN_CLOSE_WRITE),
		scm_c_make_gsubr("edit_watch_handler", 2, 0, 0, edit_watch_handler));
	}
*/

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

static SCM kv_open(SCM entry) {
	static char path[PATH_MAX];
	char *sentry;
	KVDB_NODE *node;
	KCDB *db;
	sentry = scm_to_locale_string(entry);
	sprintf(path, "%s/%s.kch", KC_ROOT, sentry);
	db = kcdbnew();
	if (!kcdbopen(db, path, KCOWRITER | KCOCREATE)) {
		log_msg("kv-open '%s': %s\n", sentry,
					kcecodename(kcdbecode(db)));
		kcdbdel(db);
		free(sentry);
		return SCM_BOOL_F;
		}
	free(sentry);
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
		kcdbclose(node->db);
		kcdbdel(node->db);
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

void shutdown_cache(void) {
	FILE_NODE *next;
	while (file_nodes != NULL) {
		next = file_nodes->next;
		free(file_nodes);
		file_nodes = next;
		}
	kv_close(sessions_db);
	return;
	}

void init_cache(void) {
	make_node_tag = scm_make_smob_type("make-node", sizeof(MAKE_NODE));
	scm_set_smob_free(make_node_tag, free_node);
	scm_set_smob_mark(make_node_tag, mark_node);
	kvdb_node_tag = scm_make_smob_type("kvdb-node", sizeof(KVDB_NODE));
	scm_set_smob_free(kvdb_node_tag, free_kvdb);
	sessions_db = kv_open(scm_from_locale_string("sessions"));
	scm_c_define("sessions-db", sessions_db);
	file_sym = scm_from_utf8_symbol("file");
	data_sym = scm_from_utf8_symbol("data");
	scm_c_define_gsubr("make-doc", 2, 0, 0, make_doc);
	scm_c_define_gsubr("touch-doc", 1, 0, 1, touch_node);
	scm_c_define_gsubr("fetch-doc", 1, 0, 1, fetch_node);
	scm_c_define_gsubr("kv-open", 1, 0, 0, kv_open);
	scm_c_define_gsubr("kv-close", 1, 0, 0, kv_close);
	scm_c_define_gsubr("kv-set", 3, 0, 0, kv_set);
	scm_c_define_gsubr("kv-get", 2, 0, 0, kv_get);
	scm_c_define_gsubr("kv-exists", 2, 0, 0, kv_exists);
	scm_c_define_gsubr("kv-count", 1, 0, 0, kv_count);
	scm_c_define_gsubr("kv-keys", 1, 0, 0, kv_keys);
	scm_c_define_gsubr("kv-del", 2, 0, 0, kv_del);
//	scm_c_define_gsubr("watch-edit", 1, 0, 0, watch_edit);
	}
