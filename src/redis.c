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

#define DEFAULT_REDIS_PORT 6379

static int redis_sock = -1;
static int redis_port;

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
	send_header("SET", 2);
	send_arg(ckey = scm_to_locale_string(key));
	send_arg(cvalue = scm_to_locale_string(value));
	free(ckey);
	free(cvalue);
	getrline(NULL);
	return SCM_BOOL_T;
	}

static SCM redis_hset(SCM key, SCM field, SCM value) {
	char *ckey, *cvalue;
	if (redis_sock < 0) return SCM_BOOL_F;
	send_header("HSET", 3);
	send_arg(ckey = scm_to_locale_string(key));
	free(ckey);
	send_arg(ckey = scm_to_locale_string(field));
	free(ckey);
	send_arg(cvalue = scm_to_locale_string(value));
	free(cvalue);
	getrline(NULL);
	return SCM_BOOL_T;
	}

static SCM redis_append(SCM key, SCM value) {
	char *ckey, *cvalue;
	if (redis_sock < 0) return SCM_BOOL_F;
	send_header("APPEND", 2);
	send_arg(ckey = scm_to_locale_string(key));
	send_arg(cvalue = scm_to_locale_string(value));
	free(ckey);
	free(cvalue);
	getrline(NULL);
	return SCM_BOOL_T;
	}

static SCM redis_get(SCM key) {
	char *ckey;
	char cmd[256], *value;
	int size;
	if (redis_sock < 0) return SCM_BOOL_F;
	send_header("GET", 1);
	send_arg(ckey = scm_to_locale_string(key));
	free(ckey);
	getrline(cmd);
	if ((size = atoi(&cmd[1])) < 0) return SCM_BOOL_F;
	value = (char *)malloc(size + 1);
	recv(redis_sock, value, size, 0);
	value[size] = '\0';
	recv(redis_sock, cmd, 2, 0);
	return scm_take_locale_string(value);
	}

static SCM redis_hget(SCM key, SCM field) {
	char *ckey;
	char cmd[256], *value;
	int size;
	if (redis_sock < 0) return SCM_BOOL_F;
	send_header("HGET", 2);
	send_arg(ckey = scm_to_locale_string(key));
	free(ckey);
	send_arg(ckey = scm_to_locale_string(field));
	free(ckey);
	getrline(cmd);
	if ((size = atoi(&cmd[1])) < 0) return SCM_BOOL_F;
	value = (char *)malloc(size + 1);
	recv(redis_sock, value, size, 0);
	value[size] = '\0';
	recv(redis_sock, cmd, 2, 0);
	return scm_take_locale_string(value);
	}

static SCM redis_get_file(SCM path) {
	char *spath, *buf, cmd[12];
	struct stat bstat;
	SCM key, content;
	int fd, n;
	if (redis_sock < 0) return SCM_BOOL_F;
	key = scm_from_locale_string("file-cache");
	spath = scm_to_locale_string(path);
	send_header("HEXISTS", 2);
	send_arg("file-cache");
	send_arg(spath);
	getrline(cmd);
	if (atoi(&cmd[1]) == 1) {
fprintf(stderr, "load %s from cache\n", spath);
		free(spath);
		return redis_hget(key, path);
		}
	if (stat(spath, &bstat) != 0) {
		free(spath);
		perror("redis-get-file[1]");
		return SCM_BOOL_F;
		}
	if ((fd = open(spath, O_RDONLY)) < 0) {
		free(spath);
		perror("redis-get-file[2]");
		return SCM_BOOL_F;
		}
	buf = (char *)malloc(bstat.st_size + 1);
	n = read(fd, (void *)buf, bstat.st_size);
	buf[n] = '\0';
	close(fd);
	content = scm_take_locale_string(buf);
	redis_hset(key, path, content);
fprintf(stderr, "load %s from FS\n", spath);
	free(spath);
	return content;
	}

static SCM redis_del(SCM key) {
	char *ckey;
	char cmd[256];
	if (redis_sock < 0) return SCM_BOOL_F;
	send_header("DEL", 1);
	send_arg(ckey = scm_to_locale_string(key));
	free(ckey);
	getrline(cmd);
	return scm_from_signed_integer(atoi(&cmd[1]));
	}

static SCM redis_exists(SCM key) {
	char *ckey;
	char cmd[256];
	if (redis_sock < 0) return SCM_BOOL_F;
	send_header("EXISTS", 1);
	send_arg(ckey = scm_to_locale_string(key));
	free(ckey);
	getrline(cmd);
	if (atoi(&cmd[1]) == 1) return SCM_BOOL_T;
	return SCM_BOOL_F;
	}

static SCM redis_ping(void) {
	char reply[64];
	if (redis_sock < 0) return SCM_BOOL_F;
	send_header("PING", 0);
	getrline(reply);
	if (strcmp(reply, "+PONG") == 0) return SCM_BOOL_T;
	return SCM_BOOL_F;
	}

void init_redis(void) {
	redis_port = DEFAULT_REDIS_PORT;
	redis_connect();
	scm_c_define_gsubr("redis-set", 2, 0, 0, redis_set);
	scm_c_define_gsubr("redis-hset", 3, 0, 0, redis_hset);
	scm_c_define_gsubr("redis-append", 2, 0, 0, redis_append);
	scm_c_define_gsubr("redis-get", 1, 0, 0, redis_get);
	scm_c_define_gsubr("redis-hget", 2, 0, 0, redis_hget);
	scm_c_define_gsubr("redis-get-file", 1, 0, 0, redis_get_file);
	scm_c_define_gsubr("redis-del", 1, 0, 0, redis_del);
	scm_c_define_gsubr("redis-ping", 0, 0, 0, redis_ping);
	scm_c_define_gsubr("redis-exists", 1, 0, 0, redis_exists);
	}
