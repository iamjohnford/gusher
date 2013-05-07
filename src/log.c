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
#include <time.h>
#include <stdarg.h>

static FILE *logfile;
static char *logpath = NULL;
static SCM lmutex;

void log_msg(const char *format, ...) {
	va_list args;
	time_t now;
	struct tm *lt;
	scm_lock_mutex(lmutex);
	now = time(NULL);
	lt = localtime(&now);
	fprintf(logfile, "%d-%02d-%02d %02d:%02d:%02d ",
		lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
		lt->tm_hour, lt->tm_min, lt->tm_sec);
	va_start(args, format);
	vfprintf(logfile, format, args);
	va_end(args);
	fflush(logfile);
	scm_unlock_mutex(lmutex);
	return;
	}

SCM log_msg_scm(SCM message) {
	char *msg;
	msg = scm_to_locale_string(message);
	log_msg("%s\n", msg);
	free(msg);
	scm_remember_upto_here_1(message);
	return SCM_BOOL_T;
	}

static SCM log_to(SCM path) {
	if (logfile != stderr) fclose(logfile);
	if (logpath != NULL) free(logpath);
	logpath = scm_to_locale_string(path);
	logfile = fopen(logpath, "w");
	if (logfile == NULL) {
		logfile = stderr;
		perror("log_to");
		free(logpath);
		logpath = NULL;
		}
	scm_remember_upto_here_1(path);
	return SCM_BOOL_T;
	}

void init_log() {
	logfile = stderr;
	scm_c_define_gsubr("log-to", 1, 0, 0, log_to);
	scm_c_define_gsubr("log-msg", 1, 0, 0, log_msg_scm);
	scm_permanent_object(lmutex = scm_make_mutex());
	}

void shutdown_log() {
	if (logfile != stderr) {
		fclose(logfile);
		logfile = stderr;
		}
	if (logpath != NULL) {
		free(logpath);
		logpath	= NULL;
		}
	}
