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
#include <math.h>

struct g_time {
	struct tm time;
	int msec;
	time_t epoch;
	};

scm_t_bits time_tag;

SCM local_time_intern(int year, int month, int day,
			int hour, int minute, double second) {
	SCM smob;
	struct tm pad;
	int msec;
	time_t epoch;
	struct g_time *time;
	pad.tm_year = year - 1900;
	pad.tm_mon = month - 1;
	pad.tm_mday = day;
	pad.tm_hour = hour;
	pad.tm_min = minute;
	pad.tm_sec = (int)second;
	pad.tm_isdst = -1;
	msec = (int)((second - pad.tm_sec) * 1000 + 0.5);
	if (msec >= 1000) {
		pad.tm_sec += 1;
		msec -= 1000;
		}
	if ((epoch = mktime(&pad)) < 0) return SCM_BOOL_F;
	time = (struct g_time *)scm_gc_malloc(sizeof(struct g_time),
					"timestamp");
	memcpy(&(time->time), &pad, sizeof(struct tm));
	time->epoch = epoch;
	time->msec = msec;
	SCM_NEWSMOB(smob, time_tag, time);
	return smob;
	}

SCM time_at(SCM epoch) {
	SCM smob;
	int msec;
	time_t ep;
	double dep;
	struct g_time *time;
	dep = scm_to_double(epoch);
	ep = (int)floor(dep);
	msec = (int)((dep - ep) * 1000 + 0.5);
	if (msec >= 1000) {
		ep += 1;
		msec -= 1000;
		}
	time = (struct g_time *)scm_gc_malloc(sizeof(struct g_time),
					"timestamp");
	localtime_r(&ep, &(time->time));
	time->epoch = ep;
	time->msec = msec;
	SCM_NEWSMOB(smob, time_tag, time);
	return smob;
	}

static SCM local_time(SCM year, SCM month, SCM day,
			SCM hour, SCM minute, SCM second) {
	return local_time_intern(scm_to_int(year),
				scm_to_int(month),
				scm_to_int(day),
				scm_to_int(hour),
				scm_to_int(minute),
				scm_to_double(second));
	}

static SCM now_time(void) {
	struct tm ltime;
	time_t utime;
	struct g_time *gtime;
	struct timeval tp;
	SCM smob;
	gtime = (struct g_time *)scm_gc_malloc(sizeof(struct g_time),
					"timestamp");
	gettimeofday(&tp, NULL);
	utime = tp.tv_sec;
	gtime->msec = (tp.tv_usec + 500) / 1000;
	if (gtime->msec >= 1000) {
		utime += 1;
		gtime->msec -= 1000;
		}
	//utime = time(NULL);
	localtime_r(&utime, &ltime);
	gtime->epoch = utime;
	memcpy(&(gtime->time), &ltime, sizeof(struct tm));
	SCM_NEWSMOB(smob, time_tag, gtime);
	return smob;
	}

SCM format_time(SCM time, SCM format) {
	SCM ftime;
	struct g_time *gtime;
	char *fmt, buf[256];
	scm_assert_smob_type(time_tag, time);
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	fmt = scm_to_locale_string(format);
	scm_remember_upto_here_1(format);
	strftime(buf, 256, (const char *)fmt, &(gtime->time));
	scm_remember_upto_here_1(time);
	ftime = scm_from_locale_string(buf);
	free(fmt);
	scm_remember_upto_here_1(ftime);
	return ftime;
	}

inline double epoch_sec(struct g_time *time) {
	return (time->epoch + time->msec / 1000.0);
	}

static SCM time_diff(SCM time1, SCM time2) {
	struct g_time *gtime1, *gtime2;
	scm_assert_smob_type(time_tag, time1);
	gtime1 = (struct g_time *)SCM_SMOB_DATA(time1);
	scm_assert_smob_type(time_tag, time2);
	gtime2 = (struct g_time *)SCM_SMOB_DATA(time2);
	return scm_from_double(epoch_sec(gtime1) - epoch_sec(gtime2));
	}

static SCM time_add(SCM time, SCM sec) {
	struct g_time *gtime;
	SCM smob;
	time_t ntime;
	struct tm ltime;
	double dtime;
	int msec;
	scm_assert_smob_type(time_tag, time);
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	dtime = epoch_sec(gtime) + scm_to_double(sec);
	ntime = (time_t)floor(dtime);
	msec = (int)((dtime - ntime) * 1000 + 0.5);
	if (msec >= 1000) {
		ntime += 1;
		msec -= 1000;
		}
	localtime_r(&ntime, &ltime);
	gtime = (struct g_time *)scm_gc_malloc(sizeof(struct g_time),
					"timestamp");
	memcpy(&(gtime->time), &ltime, sizeof(struct tm));
	gtime->msec = msec;
	gtime->epoch = ntime;
	SCM_NEWSMOB(smob, time_tag, gtime);
	return smob;
	}

static SCM time_year(SCM time) {
	struct g_time *gtime;
	scm_assert_smob_type(time_tag, time);
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_year + 1900);
	}

static SCM time_month(SCM time) {
	struct g_time *gtime;
	scm_assert_smob_type(time_tag, time);
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_mon + 1);
	}

static SCM time_mday(SCM time) {
	struct g_time *gtime;
	scm_assert_smob_type(time_tag, time);
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_mday);
	}

static SCM time_wday(SCM time) {
	struct g_time *gtime;
	int wday;
	scm_assert_smob_type(time_tag, time);
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	if ((wday = gtime->time.tm_wday) == 0) wday = 7;
	return scm_from_signed_integer(wday);
	}

static SCM time_hour(SCM time) {
	struct g_time *gtime;
	scm_assert_smob_type(time_tag, time);
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_hour);
	}

static SCM time_min(SCM time) {
	struct g_time *gtime;
	scm_assert_smob_type(time_tag, time);
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_min);
	}

static SCM time_sec(SCM time) {
	struct g_time *gtime;
	scm_assert_smob_type(time_tag, time);
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_double(gtime->time.tm_sec + gtime->msec / 1000.0);
	}

static SCM time_epoch(SCM time) {
	struct g_time *gtime;
	scm_assert_smob_type(time_tag, time);
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_int(gtime->epoch);
	}

static SCM time_offset(SCM time) {
	struct g_time *gtime;
	scm_assert_smob_type(time_tag, time);
	gtime = (struct g_time *)SCM_SMOB_DATA(time);
	return scm_from_signed_integer(gtime->time.tm_gmtoff);
	}

static SCM snooze(SCM sec) {
	double naptime, dsec;
	struct timespec ts;
	naptime = scm_to_double(sec);
	ts.tv_sec = (time_t)(dsec = floor(naptime));
	ts.tv_nsec = (naptime - dsec) * 1000000000;
	return (nanosleep(&ts, NULL) == 0 ? SCM_BOOL_T : SCM_BOOL_F);
	}

void init_time(void) {
	time_tag = scm_make_smob_type("timestamp", sizeof(struct g_time));
	scm_c_define_gsubr("time-local", 6, 0, 0, local_time);
	scm_c_define_gsubr("time-now", 0, 0, 0, now_time);
	scm_c_define_gsubr("time-at", 1, 0, 0, time_at);
	scm_c_define_gsubr("time-format", 2, 0, 0, format_time);
	scm_c_define_gsubr("time-diff", 2, 0, 0, time_diff);
	scm_c_define_gsubr("time-add", 2, 0, 0, time_add);
	scm_c_define_gsubr("time-year", 1, 0, 0, time_year);
	scm_c_define_gsubr("time-month", 1, 0, 0, time_month);
	scm_c_define_gsubr("time-mday", 1, 0, 0, time_mday);
	scm_c_define_gsubr("time-wday", 1, 0, 0, time_wday);
	scm_c_define_gsubr("time-hour", 1, 0, 0, time_hour);
	scm_c_define_gsubr("time-min", 1, 0, 0, time_min);
	scm_c_define_gsubr("time-sec", 1, 0, 0, time_sec);
	scm_c_define_gsubr("time-epoch", 1, 0, 0, time_epoch);
	scm_c_define_gsubr("time-gmtoffset", 1, 0, 0, time_offset);
	scm_c_define_gsubr("snooze", 1, 0, 0, snooze);
	} 
