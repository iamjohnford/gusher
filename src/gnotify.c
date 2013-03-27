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
#include <unistd.h>

static int infd = -1;

SCM add_watch(SCM path, SCM mask, SCM handler) {
	char *spath;
	uint32_t imask;
	struct stat bstat;
	int wd;
	SCM tuple;
	spath = scm_to_locale_string(path);
	if (stat(spath, &bstat) != 0) {
		free(spath);
		perror("add_watch[1]");
		return SCM_BOOL_F;
		}
	imask = scm_to_uint32(mask);
	wd = inotify_add_watch(infd, spath, imask);
	if (wd < 0) {
		perror("add_watch[2]");
		free(spath);
		return SCM_BOOL_F;
		}
	tuple = SCM_EOL;
	tuple = scm_cons(handler, tuple);
	tuple = scm_cons(scm_from_int(wd), tuple);
	tuple = scm_cons(path, tuple);
	scm_c_define("inotify-watched", scm_cons(tuple,
			scm_c_eval_string("inotify-watched")));
	free(spath);
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
			inotify_rm_watch(infd, iwd);
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

static char *read_event(int fd) {
	int want, got;
	char *buf, *pt;
	struct inotify_event *ev;
	char hdr[256];
	read(fd, hdr, sizeof(hdr));
	ev = (struct inotify_event *)hdr;
	want = ev->len;
	buf = (char *)malloc(sizeof(struct inotify_event) + want);
	memcpy(buf, hdr, sizeof(struct inotify_event));
	pt = ((struct inotify_event *)buf)->name;
	while (want > 0) {
		got = read(fd, pt, want);
		want -= got;
		pt += got;
		}
	return buf;
	}

static SCM watch(void *data) {
	fd_set fds;
	char *path, *fpath;
	struct inotify_event *event;
	int nfds;
	SCM node, tuple, proc;
	nfds = infd + 8;
	while (1) {
		FD_ZERO(&fds);
		FD_SET(infd, &fds);
		select(nfds, &fds, NULL, NULL, NULL);
		event = (struct inotify_event *)read_event(infd);
		node = scm_c_eval_string("inotify-watched");
		while (node != SCM_EOL) {
			tuple = SCM_CAR(node);
			if (event->wd == scm_to_int(SCM_CADR(tuple))) {
				path = scm_to_locale_string(SCM_CAR(tuple));
				proc = SCM_CADDR(tuple);
				fpath = (char *)malloc(strlen(path) +
						strlen(event->name) + 2);
				sprintf(fpath, "%s/%s", path, event->name);
				scm_call_2(proc,
					scm_take_locale_string(fpath),
					scm_from_uint32(event->mask)
					);
				free(path);
				break;
				}
			node = SCM_CDR(node);
			}
		free(event);
		}
	return SCM_UNSPECIFIED;
	}

void init_inotify() {
	infd = inotify_init();
	scm_c_define("inotify-watched", SCM_EOL);
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
	scm_spawn_thread(watch, NULL, NULL, NULL);
	}

void shutdown_inotify() {
	if (infd > 0) close(infd);
	}

