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

#include "gnotify.h"
#include "log.h"
#include "json.h"

#define DEFAULT_REDIS_PORT 6379
#define FILE_CACHE "file-cache"
#define TYPE_DATUM 0
#define TYPE_FILE 1
#define TYPE_CHAIN 2

static scm_t_bits make_node_tag;
static const char *sessions_key = "SESSIONS";

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

static int redis_sock = -1;
static int redis_port;
static SCM file_sym;
static SCM data_sym;
static SCM redis_mutex;
static FILE_NODE *file_nodes = NULL;

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

static void redis_connect(void) {
	struct sockaddr_in remote;
	redis_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	remote.sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1",
			(void *)(&(remote.sin_addr.s_addr)));
	remote.sin_port = htons(redis_port);
	if (connect(redis_sock, (struct sockaddr *)&remote,
			sizeof(struct sockaddr)) < 0) {
		close(redis_sock);
		redis_sock = -1;
		}
	return;
	}

static void chunk_send(const char *chunk) {
	int len, sent, n;
	len = strlen(chunk);
	sent = 0;
	while (sent < len) {
		n = send(redis_sock, chunk + sent, len - sent, 0);
		sent += n;
		}
	return;
	}

static void send_header(const char *cmd, int args) {
	char buf[64];
	sprintf(buf, "*%d\r\n$%ld\r\n%s\r\n", args + 1, strlen(cmd), cmd);
	chunk_send(buf);
	return;
	}

static void send_arg(const char *arg) {
	char fmt[32];
	sprintf(fmt, "$%ld\r\n", strlen(arg));
	chunk_send(fmt);
	chunk_send(arg);
	chunk_send("\r\n");
	return;
	}

static void getrline(char *buf) {
	int i;
	char c;
	i = 0;
	do {
		recv(redis_sock, &c, 1, 0);
		if (buf != NULL) buf[i++] = c;
		} while (c != '\n');
	if (buf != NULL) buf[i - 2] = '\0';
	return;
	}

/*
static char *redis_request(const char *cmd, char *response) {
	int size;
	char reply[1024];
	chunk_send(cmd);
	getrline(reply);
	switch (reply[0]) {
		case '+':
		case '-':
		case ':':
			if (response == NULL)
				response = (char *)malloc(strlen(reply) + 1);
			strcpy(response, reply);
			return response;
		case '$':
			size = atoi(&reply[1]);
			if (size < 0) return NULL;
			if (response == NULL)
				response = (char *)malloc(size + 1);
			recv(redis_sock, response, size, 0);
			response[size] = '\0';
			recv(redis_sock, reply, 2, 0);
			return response;
		case '*':
			return response;
		}
	return response;
	}
*/

static SCM redis_set(SCM key, SCM value) {
	char *ckey, *cvalue;
	if (redis_sock < 0) return SCM_BOOL_F;
	scm_lock_mutex(redis_mutex);
	send_header("SET", 2);
	send_arg(ckey = scm_to_locale_string(key));
	send_arg(cvalue = scm_to_locale_string(value));
	free(ckey);
	free(cvalue);
	getrline(NULL);
	scm_unlock_mutex(redis_mutex);
	return SCM_BOOL_T;
	}

static int redis_hset_c(const char *key, const char *field,
						const char *value) {
	if (redis_sock < 0) return 0;
	scm_lock_mutex(redis_mutex);
	send_header("HSET", 3);
	send_arg(key);
	send_arg(field);
	send_arg(value);
	getrline(NULL);
	scm_unlock_mutex(redis_mutex);
	return 1;
	}

static SCM redis_hset(SCM key, SCM field, SCM value) {
	char *ckey, *cfield, *cvalue;
	int res;
	ckey = scm_to_locale_string(key);
	cfield = scm_to_locale_string(field);
	cvalue = scm_to_locale_string(value);
	res = redis_hset_c(ckey, cfield, cvalue);
	free(ckey);
	free(cfield);
	free(cvalue);
	return (res ? SCM_BOOL_T : SCM_BOOL_F);
	}

static SCM redis_append(SCM key, SCM value) {
	char *ckey, *cvalue;
	if (redis_sock < 0) return SCM_BOOL_F;
	scm_lock_mutex(redis_mutex);
	send_header("APPEND", 2);
	send_arg(ckey = scm_to_locale_string(key));
	send_arg(cvalue = scm_to_locale_string(value));
	free(ckey);
	free(cvalue);
	getrline(NULL);
	scm_unlock_mutex(redis_mutex);
	return SCM_BOOL_T;
	}

static SCM redis_get(SCM key) {
	char *ckey;
	char cmd[256], *value;
	int size;
	if (redis_sock < 0) return SCM_BOOL_F;
	scm_lock_mutex(redis_mutex);
	send_header("GET", 1);
	send_arg(ckey = scm_to_locale_string(key));
	free(ckey);
	getrline(cmd);
	if ((size = atoi(&cmd[1])) < 0) {
		scm_unlock_mutex(redis_mutex);
		return SCM_BOOL_F;
		}
	value = (char *)malloc(size + 1);
	recv(redis_sock, value, size, 0);
	value[size] = '\0';
	recv(redis_sock, cmd, 2, 0);
	SCM out = scm_take_locale_string(value);
	scm_unlock_mutex(redis_mutex);
	return out;
	}

static char *redis_hget_c(const char *key, const char *field) {
	char cmd[256], *value;
	int size;
	if (redis_sock < 0) return NULL;
	scm_lock_mutex(redis_mutex);
	send_header("HGET", 2);
	send_arg(key);
	send_arg(field);
	getrline(cmd);
	if ((size = atoi(&cmd[1])) < 0) {
		scm_unlock_mutex(redis_mutex);
		return NULL;
		}
	value = (char *)malloc(size + 1);
	recv(redis_sock, value, size, 0);
	value[size] = '\0';
	recv(redis_sock, cmd, 2, 0);
	scm_unlock_mutex(redis_mutex);
	return value;
	}

SCM get_session(const char *sesskey) {
	char *value;
	value = redis_hget_c(sessions_key, sesskey);
	if (value == NULL) return SCM_BOOL_F;
	return json_decode(scm_take_locale_string(value));
	}

SCM put_session(const char *sesskey, SCM table) {
	int res;
	char *buf;
	buf = scm_to_locale_string(json_encode(table));
	res = redis_hset_c(sessions_key, sesskey, buf);
	free(buf);
	scm_remember_upto_here_1(table);
	return (res ? SCM_BOOL_T : SCM_BOOL_F);
	}

static SCM redis_hget(SCM key, SCM field) {
	char *ckey, *cfield, *value;
	ckey = scm_to_locale_string(key);
	cfield = scm_to_locale_string(field);
	value = redis_hget_c(ckey, cfield);
	free(ckey);
	free(cfield);
	if (value == NULL) return SCM_BOOL_F;
	return scm_take_locale_string(value);
	}

static SCM redis_get_file(SCM path) {
	char *spath, *buf, cmd[12];
	struct stat bstat;
	SCM key, content;
	int fd, n;
	if (redis_sock < 0) return SCM_BOOL_F;
	key = scm_from_locale_string(FILE_CACHE);
	spath = scm_to_locale_string(path);
	scm_lock_mutex(redis_mutex);
	send_header("HEXISTS", 2);
	send_arg(FILE_CACHE);
	send_arg(spath);
	getrline(cmd);
	scm_unlock_mutex(redis_mutex);
	if (atoi(&cmd[1]) == 1) {
log_msg("load %s from cache\n", spath);
		free(spath);
		return redis_hget(key, path);
		}
	if (stat(spath, &bstat) != 0) {
		free(spath);
		perror("cache-get-file[1]");
		return SCM_BOOL_F;
		}
	if ((fd = open(spath, O_RDONLY)) < 0) {
		free(spath);
		perror("cache-get-file[2]");
		return SCM_BOOL_F;
		}
	buf = (char *)malloc(bstat.st_size + 1);
	n = read(fd, (void *)buf, bstat.st_size);
	buf[n] = '\0';
	close(fd);
	content = scm_take_locale_string(buf);
	redis_hset(key, path, content);
log_msg("load %s from FS\n", spath);
	free(spath);
	scm_remember_upto_here_2(key, content);
	scm_remember_upto_here_1(path);
	return content;
	}

static SCM redis_del(SCM key) {
	char *ckey;
	char cmd[256];
	if (redis_sock < 0) return SCM_BOOL_F;
	scm_lock_mutex(redis_mutex);
	send_header("DEL", 1);
	send_arg(ckey = scm_to_locale_string(key));
	free(ckey);
	getrline(cmd);
	scm_unlock_mutex(redis_mutex);
	return scm_from_signed_integer(atoi(&cmd[1]));
	}

static SCM redis_exists(SCM key) {
	char *ckey;
	char cmd[256];
	if (redis_sock < 0) return SCM_BOOL_F;
	scm_lock_mutex(redis_mutex);
	send_header("EXISTS", 1);
	send_arg(ckey = scm_to_locale_string(key));
	free(ckey);
	getrline(cmd);
	scm_unlock_mutex(redis_mutex);
	return (atoi(&cmd[1]) == 1 ? SCM_BOOL_T : SCM_BOOL_F);
	}

static SCM redis_ping(void) {
	char reply[64];
	if (redis_sock < 0) return SCM_BOOL_F;
	scm_lock_mutex(redis_mutex);
	send_header("PING", 0);
	getrline(reply);
	scm_unlock_mutex(redis_mutex);
	return (strcmp(reply, "+PONG") == 0 ? SCM_BOOL_T : SCM_BOOL_F);
	}

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
	redis_mutex = scm_make_mutex();
	scm_c_define("redis-mutex", redis_mutex);
	redis_port = DEFAULT_REDIS_PORT;
	redis_connect();
	make_node_tag = scm_make_smob_type("make-node", sizeof(MAKE_NODE));
	scm_set_smob_free(make_node_tag, free_node);
	scm_set_smob_mark(make_node_tag, mark_node);
	file_sym = scm_from_utf8_symbol("file");
	data_sym = scm_from_utf8_symbol("data");
	scm_c_define_gsubr("make-doc", 2, 0, 0, make_doc);
	scm_c_define_gsubr("touch-doc", 1, 0, 1, touch_node);
	scm_c_define_gsubr("fetch-doc", 1, 0, 1, fetch_node);
	scm_c_define_gsubr("cache-set", 2, 0, 0, redis_set);
	scm_c_define_gsubr("cache-hset", 3, 0, 0, redis_hset);
	scm_c_define_gsubr("cache-append", 2, 0, 0, redis_append);
	scm_c_define_gsubr("cache-get", 1, 0, 0, redis_get);
	scm_c_define_gsubr("cache-hget", 2, 0, 0, redis_hget);
	scm_c_define_gsubr("cache-get-file", 1, 0, 0, redis_get_file);
	scm_c_define_gsubr("cache-del", 1, 0, 0, redis_del);
	scm_c_define_gsubr("cache-ping", 0, 0, 0, redis_ping);
	scm_c_define_gsubr("cache-exists", 1, 0, 0, redis_exists);
	scm_c_define_gsubr("watch-edit", 1, 0, 0, watch_edit);
	}
