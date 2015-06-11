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

#define PUB_PORT 5550
#define RELAY_PORT 5551
#define MAX_KEY_LEN 128

typedef struct sock_node {
	void *msg_sock;
	int poll;
	SCM responder;
	char key[MAX_KEY_LEN];
	struct sock_node *link;
	} SOCK_NODE;

static SCM relay_mutex;
static SCM msg_queue = SCM_EOL;
static char pub_endpoint[128];
static char relay_endpoint[128];
static void *ctx = NULL;
static void *publisher = NULL;
static void *relay = NULL;
static void *rclient = NULL;
static SCM touch_msg;
static SOCK_NODE *responders[MAX_POLL_ITEMS];
static int poll_dirty = 0;
static int poll_items = 0;
extern char gusher_root[];
static scm_t_bits sock_node_tag;
static SOCK_NODE *sock_nodes = NULL;

static SCM msg_publish(SCM sigkey, SCM msg) {
	SCM enc;
	char *payload, *keyed_msg, *sname;
	int rc;
	if (msg == SCM_UNDEFINED) msg = touch_msg;
	enc = json_encode(msg);
	if (enc == SCM_BOOL_F) enc = to_s(msg);
	payload = scm_to_utf8_string(enc);
	scm_remember_upto_here_1(enc);
	sname = scm_to_utf8_string(scm_symbol_to_string(sigkey));
	keyed_msg = (char *)malloc(strlen(payload) + MAX_KEY_LEN + 2);
	sprintf(keyed_msg, "%s:%s", sname, payload);
	free(payload);
	rc = zmq_send(rclient, keyed_msg, strlen(keyed_msg), 0);
	if (rc < 0) {
		log_msg("queue msg '%s'\n", sname);
		scm_lock_mutex(relay_mutex);
		msg_queue = scm_cons(scm_cons(sigkey, msg), msg_queue);
		scm_gc_protect_object(msg_queue);
		scm_unlock_mutex(relay_mutex);
		}
	else log_msg("publish %d: %s\n", rc, keyed_msg);
	free(sname);
	free(keyed_msg);
	scm_remember_upto_here_2(sigkey, msg);
	return (rc >= 0 ? SCM_BOOL_T : SCM_BOOL_F);
	}

static SCM msg_subscribe(SCM sigkey, SCM responder) {
	SOCK_NODE *node;
	char key[MAX_KEY_LEN];
	char *sname;
	int rc;
	node = (SOCK_NODE *)scm_gc_malloc(sizeof(SOCK_NODE), "sock-node");
	node->msg_sock = zmq_socket(ctx, ZMQ_SUB);
	sname = scm_to_locale_string(scm_symbol_to_string(sigkey));
	snprintf(key, sizeof(key), "%s:", sname);
	key[sizeof(key) - 1] = '\0';
	free(sname);
	scm_remember_upto_here_1(sigkey);
	zmq_setsockopt(node->msg_sock, ZMQ_SUBSCRIBE, key, strlen(key));
	node->poll = 1;
	node->responder = responder;
	node->link = sock_nodes;
	sock_nodes = node;
	rc = zmq_connect(node->msg_sock, pub_endpoint);
	if (rc != 0) {
		log_msg("zmq_bind to %s failed: %s\n",
				pub_endpoint, strerror(errno));
		}
	else log_msg("subscribe %s, key \"%s\"\n", pub_endpoint, key);
	poll_dirty = 1;
	return SCM_UNSPECIFIED;
	}

void init_messaging() {
	int major, minor, patch;
	scm_permanent_object(relay_mutex = scm_make_mutex());
	ctx = zmq_ctx_new();
	sprintf(pub_endpoint, "tcp://127.0.0.1:%d", PUB_PORT);
	sprintf(relay_endpoint, "tcp://127.0.0.1:%d", RELAY_PORT);
	publisher = zmq_socket(ctx, ZMQ_PUB);
	if (zmq_bind(publisher, pub_endpoint) >= 0) {
		log_msg("publisher bound to %s\n", pub_endpoint);
		relay = zmq_socket(ctx, ZMQ_REP);
		if (zmq_bind(relay, relay_endpoint) < 0) {
			log_msg("relay can't bind to %s: %s\n",
					relay_endpoint, zmq_strerror(errno));
			zmq_close(relay);
			relay = NULL;
			}
		else log_msg("relay bound to %s\n", relay_endpoint);
		}
	else {
		log_msg("publisher can't bind to %s: %s\n",
					pub_endpoint, zmq_strerror(errno));
		zmq_close(publisher);
		publisher = NULL;
		}
	rclient = zmq_socket(ctx, ZMQ_REQ);
	zmq_connect(rclient, relay_endpoint);
	poll_dirty = 1;
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
	scm_c_define_gsubr("msg-publish", 1, 1, 0, msg_publish);
	scm_c_define_gsubr("msg-subscribe", 2, 0, 0, msg_subscribe);
	return;
	}

static void relay_to_publisher(zmq_msg_t *msg) {
	size_t msg_size = zmq_msg_size(msg);
	void *msg_data = zmq_msg_data(msg);
	char *s = (char *)malloc(msg_size + 1);
	memcpy((void *)s, msg_data, msg_size);
	s[msg_size] = '\0';
	zmq_send(publisher, msg_data, msg_size, 0);
	log_msg("relay msg '%s'\n", s);
	zmq_send(relay, msg_data, msg_size, 0);
	free(s);
	return;
	}

static void recv_relay_reply(zmq_msg_t *msg) {
	size_t msg_size = zmq_msg_size(msg);
	void *msg_data = zmq_msg_data(msg);
	char *s = (char *)malloc(msg_size + 1);
	memcpy((void *)s, msg_data, msg_size);
	s[msg_size] = '\0';
	log_msg("reply from relay '%s'\n", s);
	free(s);
	if (!scm_is_null(msg_queue)) {
		log_msg("republish\n");
		scm_lock_mutex(relay_mutex);
		SCM qmsg = SCM_CAR(msg_queue);
		scm_gc_protect_object(msg_queue = SCM_CDR(msg_queue));
		msg_publish(SCM_CAR(qmsg), SCM_CDR(qmsg));
		scm_unlock_mutex(relay_mutex);
		scm_remember_upto_here_1(qmsg);
		}
	return;
	}

static void recv_subscribed_msg(zmq_msg_t *msg, SOCK_NODE *node) {
	size_t msg_size = zmq_msg_size(msg);
	void *msg_data = zmq_msg_data(msg);
	char *payload = index((const char *)msg_data, ':');
	if (payload != NULL) {
		payload += 1;
		msg_size -= (payload - (char *)msg_data);
		}
	else payload = msg_data;
	scm_call_1(node->responder,
		json_decode(scm_from_utf8_stringn(payload, msg_size)));
	return;
	}

void msg_process(int start, int finish, zmq_pollitem_t polls[]) {
	int i, rc;
	void *sock;
	zmq_msg_t msg;
	for (i = start; i < finish; i++) {
		if ((polls[i].revents & ZMQ_POLLIN) == 0) continue;
		sock = polls[i].socket;
		zmq_msg_init(&msg);
		rc = zmq_msg_recv(&msg, sock, 0);
		if (rc < 0) log_msg("MSG ERR: %s\n", zmq_strerror(errno));
		else if (sock == relay) relay_to_publisher(&msg);
		else if (sock == rclient) recv_relay_reply(&msg);
		else recv_subscribed_msg(&msg, responders[i - start]);
		zmq_msg_close(&msg);
		}
	return;
	}

int msg_poll_collect(int start, zmq_pollitem_t polls[]) {
	if (!poll_dirty) return start + poll_items;
	SOCK_NODE *node;
	poll_items = 0;
	node = sock_nodes;
	if (relay != NULL) {
		polls[start].socket = relay;
		polls[start].fd = poll_items;
		responders[poll_items] = NULL;
		polls[start].events = ZMQ_POLLIN;
		start += 1;
		poll_items += 1;
		}
	if (rclient != NULL) {
		polls[start].socket = rclient;
		polls[start].fd = poll_items;
		responders[poll_items] = NULL;
		polls[start].events = ZMQ_POLLIN;
		start += 1;
		poll_items += 1;
		}
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
		if (publisher != NULL) {
			zmq_close(publisher);
			publisher = NULL;
			}
		if (relay != NULL) {
			zmq_close(relay);
			relay = NULL;
			}
		if (rclient != NULL) {
			zmq_close(rclient);
			rclient = NULL;
			}
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
