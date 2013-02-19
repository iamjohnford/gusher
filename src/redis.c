
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <libguile.h>

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

static void getrline(char *buf) {
	int i;
	char c;
	i = 0;
	do {
		recv(redis_sock, &c, 1, 0);
		buf[i++] = c;
		} while (c != '\n');
	buf[i - 2] = '\0';
	return;
	}

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

static void redis_set_append(const char *key, const char *value,
				const char *template) {
	char cmd[256];
	sprintf(cmd, template, strlen(key));
	chunk_send(cmd);
	chunk_send(key);
	sprintf(cmd, "\r\n$%ld\r\n", strlen(value));
	chunk_send(cmd);
	chunk_send(value);
	chunk_send("\r\n");
	getrline(cmd);
	return;
	}

static SCM redis_set(SCM key, SCM value) {
	char *ckey, *cvalue;
	if (redis_sock < 0) return SCM_BOOL_F;
	ckey = scm_to_locale_string(key);
	cvalue = scm_to_locale_string(value);
	redis_set_append(ckey, cvalue, 
			"*3\r\n$3\r\nSET\r\n$%ld\r\n");
	free(ckey);
	free(cvalue);
	return SCM_BOOL_T;
	}

static SCM redis_append(SCM key, SCM value) {
	char *ckey, *cvalue;
	if (redis_sock < 0) return SCM_BOOL_F;
	ckey = scm_to_locale_string(key);
	cvalue = scm_to_locale_string(value);
	redis_set_append(ckey, cvalue, 
			"*3\r\n$6\r\nAPPEND\r\n$%ld\r\n");
	free(ckey);
	free(cvalue);
	return SCM_BOOL_T;
	}

static SCM redis_get(SCM key) {
	char *ckey;
	char cmd[256], *value;
	SCM string;
	int size;
	if (redis_sock < 0) return SCM_BOOL_F;
	ckey = scm_to_locale_string(key);
	sprintf(cmd, "*2\r\n$3\r\nGET\r\n$%ld\r\n", strlen(ckey));
	chunk_send(cmd);
	chunk_send(ckey);
	chunk_send("\r\n");
	free(ckey);
	getrline(cmd);
	if ((size = atoi(&cmd[1])) < 0) return SCM_BOOL_F;
	value = (char *)malloc(size + 1);
	recv(redis_sock, value, size, 0);
	value[size] = '\0';
	recv(redis_sock, cmd, 2, 0);
	string = scm_from_locale_string(value);
	free(value);
	return string;
	}

static SCM redis_del(SCM key) {
	char *ckey;
	char cmd[256];
	if (redis_sock < 0) return SCM_BOOL_F;
	ckey = scm_to_locale_string(key);
	sprintf(cmd, "*2\r\n$3\r\nDEL\r\n$%ld\r\n", strlen(ckey));
	chunk_send(cmd);
	chunk_send(ckey);
	chunk_send("\r\n");
	free(ckey);
	getrline(cmd);
	return scm_from_signed_integer(atoi(&cmd[1]));
	}

static SCM redis_ping(void) {
	char reply[64];
	if (redis_sock < 0) return SCM_BOOL_F;
	chunk_send("*1\r\n$4\r\nPING\r\n");
	getrline(reply);
	if (strcmp(reply, "+PONG") == 0) return SCM_BOOL_T;
	return SCM_BOOL_F;
	}

void init_redis(void) {
	redis_port = DEFAULT_REDIS_PORT;
	redis_connect();
	scm_c_define_gsubr("redis-set", 2, 0, 0, redis_set);
	scm_c_define_gsubr("redis-append", 2, 0, 0, redis_append);
	scm_c_define_gsubr("redis-get", 1, 0, 0, redis_get);
	scm_c_define_gsubr("redis-del", 1, 0, 0, redis_del);
	scm_c_define_gsubr("redis-ping", 0, 0, 0, redis_ping);
	}
