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

#include <stdio.h>
#include <libguile.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <zmq.h>

#include "log.h"
#include "json.h"
#include "butter.h"
#include "messaging.h"

typedef struct sock_node {
	void *msg_sock;
	int poll;
	SCM responder;
	struct sock_node *link;
	} SOCK_NODE;

static void *ctx = NULL;
static SCM touch_msg;
static SOCK_NODE *responders[MAX_POLL_ITEMS];
static int poll_dirty = 0;
static int poll_items = 0;
extern char gusher_root[];
static char signals_root[PATH_MAX];
//static zmq_pollitem_t *lsocks = NULL;
//static int nlsocks = 0;
static scm_t_bits sock_node_tag;
static SOCK_NODE *sock_nodes = NULL;

static char *get_sockpath(SCM signal, char *buf, int len) {
	char *sname;
	sname = scm_to_locale_string(scm_symbol_to_string(signal));
	snprintf(buf, len - 1, "ipc://%s/%s.zmq", signals_root, sname);
	buf[len - 1] = '\0';
	free(sname);
	return buf;
	}

static SCM msg_publisher(SCM endpoint) {
	SOCK_NODE *node;
	SCM smob;
	char sockpath[PATH_MAX];
	node = (SOCK_NODE *)scm_gc_malloc(sizeof(SOCK_NODE), "sock-node");
	node->msg_sock = zmq_socket(ctx, ZMQ_PUB);
	node->poll = 0;
	node->responder = SCM_BOOL_F;
	node->link = sock_nodes;
	sock_nodes = node;
	get_sockpath(endpoint, sockpath, sizeof(sockpath));
	scm_remember_upto_here_1(endpoint);
	zmq_connect(node->msg_sock, sockpath);
	char *fp = index(sockpath, '/') + 2;
	chmod(fp, 0660);
	SCM_NEWSMOB(smob, sock_node_tag, node);
	return smob;
	}

static SCM msg_publish(SCM sock, SCM msg) {
	SOCK_NODE *node;
	SCM enc;
	char *buf;
	node = (SOCK_NODE *)SCM_SMOB_DATA(sock);
	if (msg == SCM_UNDEFINED) msg = touch_msg;
	enc = json_encode(msg);
	if (enc == SCM_BOOL_F) enc = to_s(msg);
	buf = scm_to_utf8_string(enc);
	scm_remember_upto_here_1(enc);
	zmq_send(node->msg_sock, buf, strlen(buf), 0);
	free(buf);
	scm_remember_upto_here_2(sock, msg);
	return SCM_BOOL_T;
	}

static SCM msg_subscribe(SCM endpoint, SCM responder) {
	SOCK_NODE *node;
	char sockpath[PATH_MAX];
	int rc;
	node = (SOCK_NODE *)scm_gc_malloc(sizeof(SOCK_NODE), "sock-node");
	node->msg_sock = zmq_socket(ctx, ZMQ_SUB);
	zmq_setsockopt(node->msg_sock, ZMQ_SUBSCRIBE, NULL, 0);
	node->poll = 1;
	node->responder = responder;
	node->link = sock_nodes;
	sock_nodes = node;
	get_sockpath(endpoint, sockpath, sizeof(sockpath));
	scm_remember_upto_here_1(endpoint);
	rc = zmq_bind(node->msg_sock, sockpath);
	if (rc != 0) {
		log_msg("zmq_bind to %s failed: %s\n", sockpath, strerror(errno));
		}
	else log_msg("subscribe %s\n", sockpath);
	poll_dirty = 1;
	return SCM_UNSPECIFIED;
	}

void init_messaging() {
	int major, minor, patch;
	snprintf(signals_root, sizeof(signals_root) - 1,
				"%s/signals", gusher_root);
	signals_root[sizeof(signals_root) - 1] = '\0';
	ctx = zmq_ctx_new();
	sock_node_tag = scm_make_smob_type("sock-node", sizeof(SOCK_NODE));
	zmq_version(&major, &minor, &patch);
	log_msg("ZeroMQ version %d.%d.%d\n", major, minor, patch);
	touch_msg =
		scm_cons(
			scm_cons(
				scm_from_utf8_symbol("touch"),
				scm_from_utf8_string("touch")),
			SCM_EOL
			);
	scm_gc_protect_object(touch_msg);
	scm_c_define_gsubr("msg-publisher", 1, 0, 0, msg_publisher);
	scm_c_define_gsubr("msg-publish", 1, 1, 0, msg_publish);
	scm_c_define_gsubr("msg-subscribe", 2, 0, 0, msg_subscribe);
	return;
	}

void msg_process(int start, int finish, zmq_pollitem_t polls[]) {
	int i, rc;
	SOCK_NODE *node;
	zmq_msg_t msg;
	for (i = start; i < finish; i++) {
		if ((polls[i].revents & ZMQ_POLLIN) == 0) continue;
		node = responders[i - start];
		zmq_msg_init(&msg);
		rc = zmq_msg_recv(&msg, node->msg_sock, 0);
		if (rc < 0) log_msg("MSG ERR: %d\n", zmq_errno());
		else {
				size_t msg_size = zmq_msg_size(&msg);
				void *msg_data = zmq_msg_data(&msg);
				scm_call_1(node->responder,
					json_decode(scm_from_utf8_stringn(msg_data, msg_size)));
				}
		zmq_msg_close(&msg);
		}
	return;
	}

int msg_poll_collect(int start, zmq_pollitem_t polls[]) {
	if (!poll_dirty) return start + poll_items;
	SOCK_NODE *node;
	poll_items = 0;
	node = sock_nodes;
	while (node != NULL) {
		if (node->poll) {
			polls[start].socket = node->msg_sock;
			polls[start].fd = poll_items;
			responders[poll_items] = node;
			polls[start].events = ZMQ_POLLIN;
			start += 1;
			poll_items += 1;
			}
		node = node->link;
		}
	poll_dirty = 0;
	return start;
	}

void shutdown_messaging() {
	if (ctx != NULL) {
		SOCK_NODE *next;
		while (sock_nodes != NULL) {
			next = sock_nodes->link;
			zmq_close(sock_nodes->msg_sock);
			sock_nodes = next;
			}
		zmq_ctx_destroy(&ctx);
		ctx = NULL;
		}
	return;
	}
