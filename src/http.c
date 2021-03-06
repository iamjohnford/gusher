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

typedef struct chandle {
	CURL *handle;
	} CHANDLE;

typedef struct cnode {
	struct cnode *next;
	size_t size;
	char content[];
	} CNODE;

static SCM infix;
static scm_t_bits chandle_tag;
static CURL *encode_handle;

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
	scm_remember_upto_here_1(list);
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
	scm_remember_upto_here_1(doc);
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
	if (ctype == SCM_BOOL_F) return body;
	stype = scm_to_utf8_string(ctype);
	scm_remember_upto_here_1(ctype);
	if (matchn(stype, "text/json") ||
			matchn(stype, "application/json") ||
			matchn(stype, "application/javascript"))
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

static char *get_auth_creds(SCM args) {
	if (args == SCM_EOL) return NULL;
	SCM upwd = scm_assq_ref(args, symbol("userpwd"));
	if (upwd == SCM_BOOL_F) return NULL;
	return scm_to_utf8_string(upwd);
	}

struct qduple {
	char *key;
	char *value;
	struct qduple *link;
	};

static char *post_data(CURL *handle, SCM args) {
	if (args == SCM_EOL) return NULL;
	SCM post = scm_assq_ref(args, symbol("post"));
	if (post == SCM_BOOL_F) return NULL;
	SCM node = post;
	SCM pair;
	char *value, *cpt, *buf;
	struct qduple *duples, *duple, *pt, *next;
	size_t nduples, len, first;
	duples = NULL;
	nduples = 0;
	len = 0;
	while (node != SCM_EOL) {
		pair = SCM_CAR(node);
		duple = (struct qduple *)malloc(sizeof(struct qduple));
		nduples += 1;
		duple->key = scm_to_utf8_string(SCM_CAR(pair));
		value = scm_to_utf8_string(SCM_CDR(pair));
		duple->value = curl_easy_escape(handle, value, 0);
		free(value);
		len += strlen(duple->key) + strlen(duple->value) + 1;
		duple->link = duples;
		duples = duple;
		node = SCM_CDR(node);
		}
	buf = (char *)malloc(len + nduples);
	cpt = buf;
	pt = duples;
	first = 1;
	while (pt != NULL) {
		if (!first) *cpt++ = '&';
		first = 0;
		next = pt->link;
		strcpy(cpt, pt->key);
		cpt += strlen(pt->key);
		free(pt->key);
		*cpt++ = '=';
		strcpy(cpt, pt->value);
		cpt += strlen(pt->value);
		free(pt->value);
		free(pt);
		pt = next;
		}
	*cpt = '\0';
	return buf;
	}

static SCM http_url_encode(SCM src) {
	if (scm_is_string(src)) {
		SCM out;
		if (encode_handle != NULL) {
			char *ssrc = scm_to_utf8_string(src);
			char *enc = curl_easy_escape(encode_handle, ssrc, 0);
			out = scm_take_locale_string(enc);
			free(ssrc);
			}
		else {
			out = scm_from_locale_string("");
			log_msg("http_url_encode: curl init failed\n");
			}
		return out;
		scm_remember_upto_here_1(out);
		}
	return scm_from_locale_string("");
	}

static CURL *new_handle(const char *url) {
	CURL *handle = curl_easy_init();
	if (handle == NULL) return NULL;
	curl_easy_setopt(handle, CURLOPT_URL, url);
	curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_handler);
	curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_handler);
	curl_easy_setopt(handle, CURLOPT_POST, 0);
	return handle;
	}

static SCM http_handle(SCM url) {
	char *surl = scm_to_utf8_string(url);
	scm_remember_upto_here_1(url);
	CHANDLE *node = (CHANDLE *)scm_gc_malloc(sizeof(CHANDLE), "chandle");
	node->handle = new_handle(surl);
	free(surl);
	if (node->handle == NULL) return SCM_BOOL_F;
	SCM_RETURN_NEWSMOB(chandle_tag, node);
	}

static SCM http_get_master(SCM url, SCM args, int raw) {
	CURL *handle;
	char errbuf[CURL_ERROR_SIZE], *bag, *pt, *userpwd, *post_str;
	CURLcode res;
	CNODE *chunks, *next;
	long rescode;
	int local_handle;
	size_t tsize;
	if (SCM_SMOB_PREDICATE(chandle_tag, url)) {
		local_handle = 0;
		CHANDLE *obj = (CHANDLE *)SCM_SMOB_DATA(url);
		handle = obj->handle;
		}
	else {
		local_handle = 1;
		char *surl = scm_to_utf8_string(url);
		handle = new_handle(surl);
		free(surl);
		if (handle == NULL) {
			log_msg("http_get: curl init failed\n");
			return SCM_BOOL_F;
			}
		}
	scm_remember_upto_here_1(url);
	chunks = NULL;
	userpwd = NULL;
	post_str = NULL;
	SCM headers = SCM_EOL;
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, (void *)&chunks);
	curl_easy_setopt(handle, CURLOPT_HEADERDATA, (void *)&headers);
	curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);
	if ((post_str = post_data(handle, args)) != NULL) {
		curl_easy_setopt(handle, CURLOPT_POST, 1);
		curl_easy_setopt(handle, CURLOPT_POSTFIELDS, (void *)post_str);
		curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, strlen(post_str));
		}
	else curl_easy_setopt(handle, CURLOPT_POST, 0);
	if ((userpwd = get_auth_creds(args)) != NULL) {
		curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
		curl_easy_setopt(handle, CURLOPT_USERPWD, userpwd);
		}
	scm_remember_upto_here_1(args);
	res = curl_easy_perform(handle);
	free(userpwd);
	free(post_str);
	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &rescode);
	headers = scm_acons(symbol("status"), scm_from_int((int)rescode),
				headers);
	if (local_handle) curl_easy_cleanup(handle);
	if (res != 0) {
		log_msg("http-get error: %s\n", errbuf);
		return SCM_BOOL_F;
		}
	SCM body = SCM_EOL;
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
	SCM reply = scm_cons(headers, raw ? body : process_body(headers, body));
	scm_remember_upto_here_1(headers);
	scm_remember_upto_here_1(body);
	return reply;
	scm_remember_upto_here_1(reply);
	}

static SCM http_get(SCM url, SCM args) {
	return http_get_master(url, args, 0);
	}

static SCM http_get_raw(SCM url, SCM args) {
	return http_get_master(url, args, 1);
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

static size_t free_chandle(SCM smob) {
	CHANDLE *obj = (CHANDLE *)SCM_SMOB_DATA(smob);
	if (obj->handle != NULL) {
		curl_easy_cleanup(obj->handle);
		obj->handle = NULL;
		}
	return 0;
	}

void init_http() {
	curl_global_init(CURL_GLOBAL_ALL);
	chandle_tag = scm_make_smob_type("chandle", sizeof(CHANDLE));
	scm_set_smob_free(chandle_tag, free_chandle);
	infix = symbol("infix");
	scm_gc_protect_object(infix);
	scm_c_define_gsubr("http-handle", 1, 0, 0, http_handle);
	scm_c_define_gsubr("http-get", 1, 0, 1, http_get);
	scm_c_define_gsubr("http-get-raw", 1, 0, 1, http_get_raw);
	scm_c_define_gsubr("http-req", 1, 0, 1, http_get);
	scm_c_define_gsubr("http-url-encode", 1, 0, 0, http_url_encode);
	scm_c_define_gsubr("xml-node-name", 1, 0, 0, xml_node_name);
	scm_c_define_gsubr("xml-node-attrs", 1, 0, 0, xml_node_attrs);
	scm_c_define_gsubr("xml-node-content", 1, 0, 0, xml_node_content);
	scm_c_define_gsubr("xml-parse", 1, 0, 0, parse_xml);
	encode_handle = curl_easy_init();
	}

void shutdown_http() {
	if (encode_handle != NULL) curl_easy_cleanup(encode_handle);
	curl_global_cleanup();
	}

