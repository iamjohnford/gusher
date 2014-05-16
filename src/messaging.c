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

static const char *pubsock = "ipc:///tmp/gusher.sock";

typedef struct sock_node {
	void *msg_sock;
	struct sock_node *link;
	} SOCK_NODE;

static void *ctx = NULL;
static scm_t_bits sock_node_tag;
static SOCK_NODE *sock_nodes = NULL;

static SCM msg_make_publisher(SCM endpoint) {
	SOCK_NODE *node;
	SCM smob;
	node = (SOCK_NODE *)scm_gc_malloc(sizeof(SOCK_NODE), "sock-node");
	node->msg_sock = zmq_socket(ctx, ZMQ_PUB);
	node->link = sock_nodes;
	sock_nodes = node;
	char *sep = scm_to_locale_string(endpoint);
	scm_remember_upto_here_1(endpoint);
	zmq_bind(node->msg_sock, sep);
	free(sep);
	SCM_NEWSMOB(smob, sock_node_tag, node);
	return smob;
	}

static SCM msg_publish(SCM sock, SCM channel, SCM msg) {
	SOCK_NODE *node;
	SCM enc;
	char *buf;
	node = (SOCK_NODE *)SCM_SMOB_DATA(sock);
	enc = json_encode(msg);
	if (enc == SCM_BOOL_F) enc = to_s(msg);
	buf = scm_to_locale_string(channel);
	scm_remember_upto_here_1(channel);
	zmq_send(node->msg_sock, buf, strlen(buf), ZMQ_SNDMORE);
	free(buf);
	buf = scm_to_utf8_string(enc);
	scm_remember_upto_here_1(enc);
	zmq_send(node->msg_sock, buf, strlen(buf), 0);
printf("SEND %s\n", buf);
	free(buf);
	scm_remember_upto_here_2(sock, msg);
	return SCM_BOOL_T;
	}

//static SCM msg_subscribe(SCM endpoint, SCM channel, SCM handler) {
//	}

void init_messaging() {
	int major, minor, patch;
	ctx = zmq_ctx_new();
	sock_node_tag = scm_make_smob_type("sock-node", sizeof(SOCK_NODE));
	zmq_version(&major, &minor, &patch);
	log_msg("ZeroMQ version %d.%d.%d\n", major, minor, patch);
	scm_c_define_gsubr("msg-make-publisher", 1, 0, 0, msg_make_publisher);
	scm_c_define_gsubr("msg-publish", 3, 0, 0, msg_publish);
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
