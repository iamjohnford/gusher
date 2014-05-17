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
#include <zmq.h>

#include "log.h"
#include "json.h"
#include "butter.h"

typedef struct sock_node {
	void *msg_sock;
	int poll;
	SCM responder;
	struct sock_node *link;
	} SOCK_NODE;

static void *ctx = NULL;
//static zmq_pollitem_t *lsocks = NULL;
//static int nlsocks = 0;
static scm_t_bits sock_node_tag;
static SOCK_NODE *sock_nodes = NULL;

static SCM msg_publisher(SCM endpoint) {
	SOCK_NODE *node;
	SCM smob;
	node = (SOCK_NODE *)scm_gc_malloc(sizeof(SOCK_NODE), "sock-node");
	node->msg_sock = zmq_socket(ctx, ZMQ_PUB);
	node->poll = 0;
	node->responder = SCM_BOOL_F;
	node->link = sock_nodes;
	sock_nodes = node;
	char *sep = scm_to_locale_string(endpoint);
	scm_remember_upto_here_1(endpoint);
	zmq_bind(node->msg_sock, sep);
	free(sep);
	SCM_NEWSMOB(smob, sock_node_tag, node);
	return smob;
	}

static SCM msg_publish(SCM sock, SCM msg) {
	SOCK_NODE *node;
	SCM enc;
	char *buf;
	node = (SOCK_NODE *)SCM_SMOB_DATA(sock);
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
	node = (SOCK_NODE *)scm_gc_malloc(sizeof(SOCK_NODE), "sock-node");
	node->msg_sock = zmq_socket(ctx, ZMQ_SUB);
	zmq_setsockopt(node->msg_sock, ZMQ_SUBSCRIBE, NULL, 0);
	node->poll = 1;
	node->responder = responder;
	node->link = sock_nodes;
	sock_nodes = node;
	char *ep = scm_to_locale_string(endpoint);
	zmq_connect(node->msg_sock, ep);
	free(ep);
	return SCM_UNSPECIFIED;
	}

void init_messaging() {
	int major, minor, patch;
	ctx = zmq_ctx_new();
	sock_node_tag = scm_make_smob_type("sock-node", sizeof(SOCK_NODE));
	zmq_version(&major, &minor, &patch);
	log_msg("ZeroMQ version %d.%d.%d\n", major, minor, patch);
	scm_c_define_gsubr("msg-publisher", 1, 0, 0, msg_publisher);
	scm_c_define_gsubr("msg-publish", 2, 0, 0, msg_publish);
	scm_c_define_gsubr("msg-subscribe", 2, 0, 0, msg_subscribe);
	return;
	}

void msg_poll() {
	SOCK_NODE *node;
	zmq_msg_t msg;
	int rc, err;
	node = sock_nodes;
	while (node != NULL) {
		if (node->poll) {
			zmq_msg_init(&msg);
			rc = zmq_msg_recv(&msg, node->msg_sock, ZMQ_DONTWAIT);
			if (rc < 0) {
				if ((err = zmq_errno()) != EAGAIN) {
					log_msg("MSG ERR: %d\n", err);
					}
				}
			else {
					size_t msg_size = zmq_msg_size(&msg);
					void *msg_data = zmq_msg_data(&msg);
					scm_call_1(node->responder,
						json_decode(scm_from_utf8_stringn(msg_data, msg_size)));
					}
			zmq_msg_close(&msg);
			}
		node = node->link;
		}
	return;
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
	}
