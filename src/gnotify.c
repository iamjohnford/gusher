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

#include <sys/inotify.h>
#include <libguile.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#include "log.h"

#define SIGBUFSIZE 1024
#define MAX_SNOOZE 20.0
#define EVENT_BLOCK (sizeof(struct inotify_event)+NAME_MAX+1)
#define EVENT_BLOB (EVENT_BLOCK*16)

int inotify_fd = -1;
extern char gusher_root[];
static char signals_root[PATH_MAX];
static char events_buf[EVENT_BLOB];
static SCM watch_nodes;
static SCM mutex;
static int pulsing = 0;

SCM add_watch(SCM path, SCM mask, SCM handler) {
	char *spath;
	uint32_t imask;
	struct stat bstat;
	int wd;
	SCM tuple, newlist;
	spath = scm_to_locale_string(path);
	if (stat(spath, &bstat) != 0) {
		free(spath);
		perror("add_watch[1]");
		return SCM_BOOL_F;
		}
	imask = scm_to_uint32(mask);
	wd = inotify_add_watch(inotify_fd, spath, imask);
	if (wd < 0) {
		perror("add_watch[2]");
		free(spath);
		return SCM_BOOL_F;
		}
	tuple = SCM_EOL;
	tuple = scm_cons(handler, tuple);
	tuple = scm_cons(scm_from_int(wd), tuple);
	tuple = scm_cons(path, tuple);
	newlist = scm_cons(tuple, watch_nodes);
	scm_gc_protect_object(newlist);
	watch_nodes = newlist;
	free(spath);
	scm_remember_upto_here_2(tuple, newlist);
	scm_remember_upto_here_2(path, mask);
	scm_remember_upto_here_1(handler);
	return SCM_BOOL_T;
	}

static void write_signal(const char *path, const char *msg) {
	int fd;
	scm_lock_mutex(mutex);
	if ((fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0664)) < 0) {
		scm_unlock_mutex(mutex);
		log_msg("signals: can't write %s [%s]\n", path, strerror(errno));
		return;
		}
	flock(fd, LOCK_EX);
	fchmod(fd, 0664);
	if (msg != NULL) write(fd, (const void *)msg, strlen(msg));
	flock(fd, LOCK_UN);
	close(fd);
	scm_unlock_mutex(mutex);
	return;
	}

static int assure_sigfile(const char *path) {
	struct stat bstat;
	if (stat(path, &bstat) == 0) return 1;
	if (errno != ENOENT) {
		perror("assure_sigfile");
		return 0;
		}
	write_signal(path, NULL);
	return 1;
	}

static char *get_sigpath(SCM signal, char *buf, int len) {
	char *sname;
	sname = scm_to_locale_string(scm_symbol_to_string(signal));
	snprintf(buf, len - 1, "%s/%s", signals_root, sname);
	buf[len - 1] = '\0';
	free(sname);
	return buf;
	}

SCM signal_subscribe(SCM signal, SCM handler) {
	char sigpath[PATH_MAX];
	SCM tuple, newlist;
	int wd;
	get_sigpath(signal, sigpath, sizeof(sigpath));
	assure_sigfile(sigpath);
	wd = inotify_add_watch(inotify_fd, sigpath, IN_ATTRIB | IN_CLOSE_WRITE);
	tuple = SCM_EOL;
	tuple = scm_cons(scm_from_locale_string(sigpath), tuple);
	tuple = scm_cons(handler, tuple);
	tuple = scm_cons(scm_from_int(wd), tuple);
	newlist = scm_cons(tuple, watch_nodes);
	scm_gc_protect_object(newlist);
	if (watch_nodes != SCM_EOL) scm_gc_unprotect_object(watch_nodes);
	watch_nodes = newlist;
	scm_remember_upto_here_2(tuple, newlist);
	scm_remember_upto_here_2(signal, handler);
	return SCM_BOOL_T;
	}

SCM signal_touch(SCM signal, SCM rest) {
	char *msg;
	char sigpath[PATH_MAX];
	get_sigpath(signal, sigpath, sizeof(sigpath));
	assure_sigfile(sigpath);
	if (rest != SCM_EOL) msg = scm_to_locale_string(SCM_CAR(rest));
	else msg = NULL;
	write_signal(sigpath, msg);
	free(msg);
	return SCM_BOOL_T;
	}

static SCM rm_watch(SCM path) {
	int iwd;
	char *spath, *stpath;
	SCM node, tuple, wd, rest, tpath;
	rest = SCM_EOL;
	spath = scm_to_locale_string(path);
	node = scm_c_eval_string("inotify-watched");
	while (node != SCM_EOL) {
		tuple = SCM_CAR(node);
		tpath = SCM_CAR(tuple);
		stpath = scm_to_locale_string(tpath);
		if (strcmp(spath, stpath) == 0) {
			wd = SCM_CADR(tuple);
			iwd = scm_to_int(wd);
			inotify_rm_watch(inotify_fd, iwd);
			}
		else rest = scm_cons(tuple, rest);
		free(stpath);
		node = SCM_CDR(node);
		}
	free(spath);
	scm_c_define("inotify-watched", rest);
	return SCM_UNSPECIFIED;
	}

static void mask_const(const char *name, uint32_t val) {
	scm_c_define(name, scm_from_uint32(val));
	return;
	}

static SCM get_signal_msg_intern(const char *path) {
	int fd, n;
	SCM list, msg;
	char *buf;
	scm_lock_mutex(mutex);
	if ((fd = open(path, O_RDONLY)) < 0) {
		scm_unlock_mutex(mutex);
		buf = (char *)malloc(SIGBUFSIZE);
		snprintf(buf, SIGBUFSIZE - 1, "unable to read %s", path);
		buf[SIGBUFSIZE - 1] = '\0';
		return scm_take_locale_string(buf);
		}
	flock(fd, LOCK_SH);
	list = scm_cons(scm_from_locale_string(""), SCM_EOL);
	buf = (char *)malloc(SIGBUFSIZE);
	while ((n = read(fd, (void *)buf, SIGBUFSIZE)) > 0) {
		list = scm_cons(scm_take_locale_stringn(buf, n), list);
		buf = (char *)malloc(SIGBUFSIZE);
		}
	free(buf);
	flock(fd, LOCK_UN);
	close(fd);
	scm_unlock_mutex(mutex);
	msg = scm_string_concatenate(scm_reverse(list));
	scm_remember_upto_here_2(list, msg);
	return msg;
	}

static SCM get_signal_msg(SCM signal) {
	char path[PATH_MAX];
	get_sigpath(signal, path, sizeof(path));
	return get_signal_msg_intern(path);
	}

void process_inotify_events() {
	int n, i;
	struct inotify_event *event;
	SCM node, tuple, msg;
	char *sigfile;
	n = read(inotify_fd, events_buf, EVENT_BLOB);
	if (n < 1) return;
	if (n == EVENT_BLOB) log_msg("inotify: possible dropped events");
	i = 0;
	while (i < n) {
		event = (struct inotify_event *)(&events_buf[i]);
		sigfile = NULL;
		msg = scm_from_locale_string("");
		node = watch_nodes;
		while (node != SCM_EOL) {
			tuple = SCM_CAR(node);
			if (event->wd == scm_to_int(SCM_CAR(tuple))) {
				if (sigfile == NULL) {
					sigfile = scm_to_locale_string(SCM_CADDR(tuple));
					msg = get_signal_msg_intern(sigfile);
					}
				scm_call_1(SCM_CADR(tuple), msg);
				}
			node = SCM_CDR(node);
			}
		free(sigfile);
		i += sizeof(struct inotify_event) + event->len;
		}
	return;
	}

static SCM pulsar(void *data) {
	char sigpath[PATH_MAX];
	struct timeval tod;
	struct timespec nap;
	int interval, partial;
	//time_t now;
	double pnow, wake, snooze;
	interval = *((int *)data);
	sprintf(sigpath, "%s/pulse", signals_root);
	pulsing = 1;
	while (pulsing) {
		gettimeofday(&tod, NULL);
		pnow = tod.tv_sec + tod.tv_usec / 1000000.0;
		wake = (tod.tv_sec / interval + 1) * interval;
		if ((snooze = wake - pnow) > MAX_SNOOZE) {
			partial = 1;
			snooze = MAX_SNOOZE;
			}
		else partial = 0;
		nap.tv_sec = (int)floor(snooze);
		nap.tv_nsec = (snooze - nap.tv_sec) * 1000000000;
		if ((nanosleep(&nap, NULL) == 0) && !partial) {
			write_signal(sigpath, NULL);
			/*gettimeofday(&tod, NULL);
			now = (time_t)round(tod.tv_sec + tod.tv_usec / 1000000.0);
			printf("pulse! %s", ctime(&now));*/
			}
		}
	return SCM_UNSPECIFIED;
	}

static SCM start_pulse(SCM interval) {
	int sec;
	if (pulsing) return SCM_BOOL_F;
	sec = scm_to_int(interval);
	scm_spawn_thread(pulsar, (void *)&sec, NULL, NULL);
	return SCM_BOOL_T;
	}

static SCM stop_pulse() {
	pulsing = 0;
	return SCM_UNSPECIFIED;
	}

void init_inotify() {
	inotify_fd = inotify_init();
	scm_permanent_object(mutex = scm_make_mutex());
	watch_nodes = SCM_EOL;
	snprintf(signals_root, sizeof(signals_root) - 1,
				"%s/signals", gusher_root);
	signals_root[sizeof(signals_root) - 1] = '\0';
	mask_const("inotify-modify", IN_MODIFY);
	mask_const("inotify-access", IN_ACCESS);
	mask_const("inotify-attrib", IN_ATTRIB);
	mask_const("inotify-close-no-write", IN_CLOSE_NOWRITE);
	mask_const("inotify-move-self", IN_MOVE_SELF);
	mask_const("inotify-moved-from", IN_MOVED_FROM);
	mask_const("inotify-moved-to", IN_MOVED_TO);
	mask_const("inotify-open", IN_OPEN);
	mask_const("inotify-delete-self", IN_DELETE_SELF);
	mask_const("inotify-create", IN_CREATE);
	mask_const("inotify-delete", IN_DELETE);
	mask_const("inotify-close-write", IN_CLOSE_WRITE);
	scm_c_define_gsubr("inotify-add-watch", 3, 0, 0, add_watch);
	scm_c_define_gsubr("inotify-rm-watch", 1, 0, 0, rm_watch);
	scm_c_define_gsubr("signal-subscribe", 2, 0, 0, signal_subscribe);
	scm_c_define_gsubr("signal-touch", 1, 0, 1, signal_touch);
	scm_c_define_gsubr("signal-msg", 1, 0, 0, get_signal_msg);
	scm_c_define_gsubr("start-pulse", 1, 0, 0, start_pulse);
	scm_c_define_gsubr("stop-pulse", 0, 0, 0, stop_pulse);
	}

void shutdown_inotify() {
	if (inotify_fd > 0) close(inotify_fd);
	}

