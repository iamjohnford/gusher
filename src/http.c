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
#include <libxml2/libxml/parser.h>

#include "json.h"
#include "log.h"

#define match(a,b) (strcmp(a,b) == 0)
#define symbol(s) (scm_from_utf8_symbol(s))

typedef struct hnode {
	CURL *handle;
	struct hnode *next;
	} HNODE;

typedef struct cnode {
	struct cnode *next;
	size_t size;
	char content[];
	} CNODE;

static HNODE *hpool = NULL;
static SCM mutex;
static SCM infix;

static HNODE *new_handle() {
	HNODE *node;
	node = (HNODE *)malloc(sizeof(HNODE));
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
	hpool = node->next;
	node->next = NULL;
	scm_unlock_mutex(mutex);
	return node;
	}

static void release_handle(HNODE *node) {
	scm_lock_mutex(mutex);
	node->next = hpool;
	hpool = node;
	scm_unlock_mutex(mutex);
	return;
	}

static size_t write_handler(void *buffer, size_t size,
				size_t n, void *userp) {
	size_t rsize;
	CNODE *node;
	rsize = size * n;
	node = (CNODE *)malloc(sizeof(CNODE) + rsize);
	memcpy(node->content, buffer, rsize);
	node->size = rsize;
	node->next = *((CNODE **)userp);
	*((CNODE **)userp) = node;
	return rsize;
	}

static char *downcase(char *buf) {
	char *pt;
	for (pt = buf; *pt; pt++) *pt = tolower(*pt);
	return buf;
	}

inline SCM scm_from_string(const char *str) {
	return scm_from_utf8_string(str);
	}

static size_t header_handler(void *data, size_t size,
			size_t n, void *userp) {
	char *buf, *pt, *value;
	size_t rsize;
	SCM sym, val;
	buf = (char *)data;
	rsize = size * n;
	buf[rsize] = '\0';
	if ((pt = index(buf, ':')) == NULL) return rsize;
	*pt++ = '\0';
	while (isspace(*pt)) pt++;
	sym = symbol(downcase(buf));
	value = pt;
	pt = value + strlen(value) - 1;
	while (isspace(*pt)) {
		*pt = '\0';
		if (pt == value) break;
		pt--;
		}
	val = scm_from_latin1_string(value);
	*((SCM *)userp) = scm_acons(sym, val, *((SCM *)userp));
	scm_remember_upto_here_2(sym, val);
	return rsize;
	}

static SCM trim_cat(SCM list) {
	SCM joined;
	char *buf, *pt;
	int trimmed;
	joined = scm_string_join(list, scm_from_string(""), infix);
	trimmed = 0;
	buf = scm_to_utf8_string(joined);
	pt = buf + strlen(buf) - 1;
	while (isspace(*pt)) {
		*pt-- = '\0';
		trimmed = 1;
		}
	for (pt = buf; *pt && isspace(*pt); pt++) trimmed = 1;
	joined = scm_from_string(trimmed ? pt : buf);
	free(buf);
	scm_remember_upto_here_1(joined);
	return joined;
	}

static SCM walk_tree(xmlNode *node, int level) {
	xmlNode *knode;
	xmlAttr *attr;
	const char *name;
	SCM snode, text, attribs, elements;
	if (node->name != NULL) name = (const char *)node->name;
	else name = "no-name";
	text = SCM_EOL;
	elements = SCM_EOL;
	if (node->children != NULL) {
		for (knode = node->children; knode; knode = knode->next) {
			if (knode->type == XML_ELEMENT_NODE)
				elements = scm_cons(walk_tree(knode, level + 1), elements);
			else if ((knode->type == XML_TEXT_NODE) ||
					(knode->type == XML_CDATA_SECTION_NODE)) {
				text = scm_cons(scm_from_string((const char *)
										knode->content), text);
				}
			else if (knode->type == XML_COMMENT_NODE);
			else
				fprintf(stderr, "NODE TYPE %d\n", knode->type);
			}
		}
	snode = scm_cons(
		scm_is_null(elements) ?
			trim_cat(scm_reverse(text)) :
			scm_reverse(elements),
		SCM_EOL);
	attribs = SCM_EOL;
	if (node->properties != NULL) {
		for (attr = node->properties; attr; attr = attr->next) {
			attribs = scm_acons(symbol((const char *)attr->name),
					scm_from_string((const char *)
								attr->children->content),
					attribs);
			}
		}
	snode = scm_cons(attribs, snode);
	snode = scm_cons(symbol(name), snode);
	scm_remember_upto_here_2(snode, elements);
	scm_remember_upto_here_2(text, attribs);
	return snode;
	}

static SCM parse_xml(SCM doc) {
	char *buf;
	SCM tree;
	xmlDoc *xmldoc;
	xmlNode *root;
	buf = scm_to_utf8_string(doc);
	xmldoc = xmlReadMemory(buf, strlen(buf), NULL, NULL,
		XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (xmldoc == NULL) {
		free(buf);
		return SCM_BOOL_F;
		}
	root = xmlDocGetRootElement(xmldoc);
	tree = walk_tree(root, 0);
	xmlFreeDoc(xmldoc);
	xmlCleanupParser();
	free(buf);
	scm_remember_upto_here_1(tree);
	return tree;
	}

inline int matchn(const char *val, const char *mime) {
	return strncmp(val, mime, strlen(mime)) == 0;
	}

static SCM process_body(SCM headers, SCM body) {
	char *stype;
	SCM ctype;
	ctype = scm_assq_ref(headers, symbol("content-type"));
	if (ctype == SCM_BOOL_F) return SCM_BOOL_F;
	stype = scm_to_utf8_string(ctype);
	scm_remember_upto_here_1(ctype);
	if (match(stype, "text/json") ||
			match(stype, "application/json") ||
			match(stype, "application/javascript"))
		body = json_decode(body);
	else if (matchn(stype, "application/xml") ||
			matchn(stype, "application/atom+xml") ||
			matchn(stype, "application/rss+xml") ||
			matchn(stype, "text/xml"))
		body = parse_xml(body);
	free(stype);
	scm_remember_upto_here_2(body, headers);
	return body;
	}

static SCM http_get(SCM url) {
	HNODE *hnode;
	CURL *handle;
	char *surl, errbuf[CURL_ERROR_SIZE], *bag, *pt;
	CURLcode res;
	CNODE *chunks, *next;
	long rescode;
	size_t tsize;
	SCM body, headers, reply;
	hnode = get_handle();
	handle = hnode->handle;
	chunks = NULL;
	headers = SCM_EOL;
	surl = scm_to_utf8_string(url);
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
	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &rescode);
	headers = scm_acons(symbol("status"), scm_from_int((int)rescode),
				headers);
	release_handle(hnode);
	if (res != 0) {
		log_msg("http-get error: %s\n", errbuf);
		return SCM_BOOL_F;
		}
	body = SCM_EOL;
	tsize = 0;
	for (next = chunks; next != NULL; next = next->next)
		tsize += next->size;
	bag = (char *)malloc(tsize + 1);
	pt = &bag[tsize];
	while (chunks != NULL) {
		next = chunks->next;
		pt -= chunks->size;
		memcpy(pt, chunks->content, chunks->size);
		free(chunks);
		chunks = next;
		}
	body = scm_from_utf8_stringn(bag, tsize);
	free(bag);
	reply = scm_cons(headers, process_body(headers, body));
	scm_remember_upto_here_1(headers);
	scm_remember_upto_here_2(body, reply);
	return reply;
	}

static SCM xml_node_name(SCM xml_doc) {
	return SCM_CAR(xml_doc);
	}

static SCM xml_node_attrs(SCM xml_doc) {
	return SCM_CAR(SCM_CDR(xml_doc));
	}

static SCM xml_node_content(SCM xml_doc) {
	return SCM_CAR(SCM_CDR(SCM_CDR(xml_doc)));
	}

void init_http() {
	curl_global_init(CURL_GLOBAL_ALL);
	infix = symbol("infix");
	scm_gc_protect_object(infix);
	scm_gc_protect_object(mutex = scm_make_mutex());
	scm_c_define_gsubr("http-get", 1, 0, 0, http_get);
	scm_c_define_gsubr("xml-node-name", 1, 0, 0, xml_node_name);
	scm_c_define_gsubr("xml-node-attrs", 1, 0, 0, xml_node_attrs);
	scm_c_define_gsubr("xml-node-content", 1, 0, 0, xml_node_content);
	}

void shutdown_http() {
	HNODE *next;
	while (hpool != NULL) {
		next = hpool->next;
		curl_easy_cleanup(hpool->handle);
		free(hpool);
		hpool = next;
		}
	curl_global_cleanup();
	}

