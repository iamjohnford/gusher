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

#include "log.h"
#include "json.h"

#define DEFAULT_REDIS_PORT 6379
#define FILE_CACHE "file-cache"
#define TYPE_DATUM 0
#define TYPE_FILE 1
#define TYPE_CHAIN 2

typedef struct make_node {
	SCM callback;
	SCM payload;
	SCM ascendants;
	SCM mutex;
	char *filepath;
	int type;
	int dirty;
	} MAKE_NODE;

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
	sessions_db = scm_from_locale_string("sessions");
	scm_gc_protect_object(sessions_db);
	scm_permanent_object(file_sym = scm_from_utf8_symbol("file"));
	scm_permanent_object(data_sym = scm_from_utf8_symbol("data"));
	scm_permanent_object(stamp_sym = scm_from_utf8_symbol("stamp"));
	scm_c_define_gsubr("make-doc", 2, 0, 0, make_doc);
	scm_c_define_gsubr("touch-doc", 1, 0, 1, touch_node);
	scm_c_define_gsubr("fetch-doc", 1, 0, 1, fetch_node);
	scm_c_define_gsubr("touched-doc?", 1, 0, 0, touched_node);
	}
