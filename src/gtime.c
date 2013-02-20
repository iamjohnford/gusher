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

#include <libguile.h>
#include <time.h>

struct g_time {
	struct tm time;
	};

static scm_t_bits time_tag;

SCM make_time_intern(int year, int month, int day,
			int hour, int minute, int second) {
	SCM smob;
	struct g_time *time;
	time = (struct g_time *)scm_gc_malloc(sizeof(struct g_time),
					"timestamp");
	time->time.tm_year = year - 1900;
	time->time.tm_mon = month - 1;
	time->time.tm_mday = day;
	time->time.tm_hour = hour;
	time->time.tm_min = minute;
	time->time.tm_sec = second;
	time->time.tm_isdst = -1;
	mktime(&(time->time));
	SCM_NEWSMOB(smob, time_tag, time);
	return smob;
	}

static SCM make_time(SCM year, SCM month, SCM day,
			SCM hour, SCM minute, SCM second) {
	return make_time_intern(scm_to_int(year),
				scm_to_int(month),
				scm_to_int(day),
				scm_to_int(hour),
				scm_to_int(minute),
				scm_to_int(second));
	}

static SCM now_time(void) {
	struct tm *ltime;
	time_t utime;
	struct g_time *gtime;
	SCM smob;
	gtime = (struct g_time *)scm_gc_malloc(sizeof(struct g_time),
					"timestamp");
	utime = time(NULL);
	ltime = localtime(&utime);
	gtime->time.tm_year = ltime->tm_year;
	gtime->time.tm_mon = ltime->tm_mon;
	gtime->time.tm_mday = ltime->tm_mday;
	gtime->time.tm_hour = ltime->tm_hour;
	gtime->time.tm_min = ltime->tm_min;
	gtime->time.tm_sec = ltime->tm_sec;
	gtime->time.tm_wday = ltime->tm_wday;
	gtime->time.tm_yday = ltime->tm_yday;
	gtime->time.tm_isdst = ltime->tm_isdst;
	SCM_NEWSMOB(smob, time_tag, gtime);
	return smob;
	}

static SCM format_time(SCM time, SCM format) {
	SCM ftime;
	struct g_time *gtime;
	char *fmt, buf[256];
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	fmt = scm_to_locale_string(format);
	strftime(buf, 256, (const char *)fmt, &(gtime->time));
	ftime = scm_from_locale_string(buf);
	free(fmt);
	return ftime;
	}

static SCM time_diff(SCM time1, SCM time2) {
	struct g_time *gtime1, *gtime2;
	int diff;
	gtime1 = (struct g_time *)SCM_SMOB_DATA(time1);
	gtime2 = (struct g_time *)SCM_SMOB_DATA(time2);
	diff = mktime(&(gtime1->time)) - mktime(&(gtime2->time));
	return scm_from_signed_integer(diff);
	}

static SCM time_add(SCM time, SCM sec) {
	struct g_time *gtime;
	SCM smob;
	time_t ntime;
	struct tm *ltime;
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	ntime = mktime(&(gtime->time)) + scm_to_int(sec);
	ltime = localtime(&ntime);
	gtime = (struct g_time *)scm_gc_malloc(sizeof(struct g_time),
					"timestamp");
	gtime->time.tm_year = ltime->tm_year;
	gtime->time.tm_mon = ltime->tm_mon;
	gtime->time.tm_mday = ltime->tm_mday;
	gtime->time.tm_hour = ltime->tm_hour;
	gtime->time.tm_min = ltime->tm_min;
	gtime->time.tm_sec = ltime->tm_sec;
	gtime->time.tm_wday = ltime->tm_wday;
	gtime->time.tm_yday = ltime->tm_yday;
	gtime->time.tm_isdst = ltime->tm_isdst;
	SCM_NEWSMOB(smob, time_tag, gtime);
	return smob;
	}

static SCM time_year(SCM time) {
	struct g_time *gtime;
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_year + 1900);
	}

static SCM time_month(SCM time) {
	struct g_time *gtime;
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_mon + 1);
	}

static SCM time_mday(SCM time) {
	struct g_time *gtime;
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_mday);
	}

static SCM time_hour(SCM time) {
	struct g_time *gtime;
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_hour);
	}

static SCM time_min(SCM time) {
	struct g_time *gtime;
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_min);
	}

static SCM time_sec(SCM time) {
	struct g_time *gtime;
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_sec);
	}

void init_time(void) {
	time_tag = scm_make_smob_type("timestamp", sizeof(struct g_time));
	scm_c_define_gsubr("time-make", 6, 0, 0, make_time);
	scm_c_define_gsubr("time-format", 2, 0, 0, format_time);
	scm_c_define_gsubr("time-now", 0, 0, 0, now_time);
	scm_c_define_gsubr("time-diff", 2, 0, 0, time_diff);
	scm_c_define_gsubr("time-add", 2, 0, 0, time_add);
	scm_c_define_gsubr("time-year", 1, 0, 0, time_year);
	scm_c_define_gsubr("time-month", 1, 0, 0, time_month);
	scm_c_define_gsubr("time-mday", 1, 0, 0, time_mday);
	scm_c_define_gsubr("time-hour", 1, 0, 0, time_hour);
	scm_c_define_gsubr("time-min", 1, 0, 0, time_min);
	scm_c_define_gsubr("time-sec", 1, 0, 0, time_sec);
	} 
