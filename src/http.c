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

#include <curl/curl.h>
#include <curl/easy.h>
#include <libguile.h>
#include <ctype.h>
#include <string.h>

#include "json.h"
#include "log.h"

typedef struct hnode {
	CURL *handle;
	struct hnode *next;
	char domain[];
	} HNODE;

static HNODE *hpool = NULL;
static SCM mutex;
static SCM content_type;
static SCM infix_grammar;

static HNODE *new_handle() {
	HNODE *node;
	node = (HNODE *)malloc(sizeof(HNODE));
fprintf(stderr, "MAKE %08x\n", node);
	node->handle = curl_easy_init();
	node->next = NULL;
	return node;
	}

static HNODE *get_handle() {
	HNODE *node;
	scm_lock_mutex(mutex);
	if (hpool == NULL) {
		node = new_handle();
		scm_unlock_mutex(mutex);
		return node;
		}
	node = hpool;
fprintf(stderr, "REUSE %08x\n", node);
	hpool = node->next;
	node->next = NULL;
	scm_unlock_mutex(mutex);
	return node;
	}

static void release_handle(HNODE *node) {
	scm_lock_mutex(mutex);
fprintf(stderr, "RELEASE %08x\n", node);
	node->next = hpool;
	hpool = node;
	scm_unlock_mutex(mutex);
	return;
	}

static size_t write_handler(void *buffer, size_t size,
							size_t n, void *userp) {
	size_t rsize;
	SCM string;
	rsize = size * n;
	string = scm_from_locale_stringn((char *)buffer, rsize);
	*((SCM *)userp) = scm_cons(string, *((SCM *)userp));
	return rsize;
	}

static char *downcase(char *buf) {
	char *pt;
	for (pt = buf; *pt; pt++) *pt = tolower(*pt);
	return buf;
	}

static size_t header_handler(void *data, size_t size, size_t n,
									void *userp) {
	char *buf, *pt, *value;
	size_t rsize;
	SCM sym, val;
	buf = (char *)data;
	rsize = size * n;
	buf[rsize] = '\0';
	if ((pt = index(buf, ':')) == NULL) return rsize;
	*pt++ = '\0';
	while (isspace(*pt)) pt++;
	sym = scm_from_locale_symbol(downcase(buf));
	value = pt;
	pt = index(value, '\r');
	if (pt == NULL) pt = index(value, '\n');
	*pt = '\0';
	val = scm_from_locale_string(value);
	*((SCM *)userp) = scm_acons(sym, val, *((SCM *)userp));
	return rsize;
	}

static SCM process_body(SCM headers, SCM body) {
	char *ctype;
	ctype = scm_to_locale_string(scm_assq_ref(headers, content_type));
	if (strcmp(ctype, "text/json") == 0) body = json_decode(body);
	free(ctype);
	scm_remember_upto_here_1(body);
	return body;
	}

static SCM http_get(SCM url) {
	HNODE *hnode;
	CURL *handle;
	char *surl, errbuf[CURL_ERROR_SIZE];
	CURLcode res;
	SCM chunks, glue, body, headers, reply;
	hnode = get_handle();
	handle = hnode->handle;
	chunks = SCM_EOL;
	headers = SCM_EOL;
	surl = scm_to_locale_string(url);
	curl_easy_setopt(handle, CURLOPT_URL, surl);
	/*curl_easy_setopt(handle, CURLOPT_HEADER, 1);
	curl_easy_setopt(handle, CURLOPT_VERBOSE, 1);*/
	curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_handler);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, (void *)&chunks);
	curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_handler);
	curl_easy_setopt(handle, CURLOPT_HEADERDATA, (void *)&headers);
	curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);
	res = curl_easy_perform(handle);
	free(surl);
	release_handle(hnode);
	if (res != 0) {
		log_msg("http-get error: %s\n", errbuf);
		return SCM_BOOL_F;
		}
	glue = scm_from_locale_string("");
	body = scm_string_join(scm_reverse(chunks), glue, infix_grammar);
	reply = scm_cons(headers, process_body(headers, body));
	scm_remember_upto_here_2(chunks, headers);
	scm_remember_upto_here_2(body, reply);
	return reply;
	}

void init_http() {
	curl_global_init(CURL_GLOBAL_ALL);
	content_type = scm_from_locale_symbol("content-type");
	infix_grammar = scm_from_locale_symbol("infix");
	mutex = scm_make_mutex();
	scm_c_define_gsubr("http-get", 1, 0, 0, http_get);
	}

void shutdown_http() {
	HNODE *next;
	while (hpool != NULL) {
fprintf(stderr, "FREE %08x\n", hpool);
		next = hpool->next;
		curl_easy_cleanup(hpool->handle);
		free(hpool);
		hpool = next;
		}
	curl_global_cleanup();
	}

