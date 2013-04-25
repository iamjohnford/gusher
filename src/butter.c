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

static SCM radix10;

static SCM to_s(SCM obj) {
	if (scm_is_string(obj)) return obj;
	if (scm_is_number(obj)) return scm_number_to_string(obj, radix10);
	if (scm_is_symbol(obj)) return scm_symbol_to_string(obj);
	if (scm_is_null(obj)) return scm_from_locale_string("");
	return obj;
	}

static SCM to_i(SCM obj) {
	int i;
	char *buf;
	i = 0;
	if (scm_is_string(obj)) {
		buf = scm_to_locale_string(obj);
		i = atoi(buf);
		free(buf);
		}
	else if (scm_is_integer(obj)) return obj;
	else if (scm_is_real(obj)) i = (int)scm_to_double(obj);
	else if (obj == SCM_BOOL_T) i = 1;
	return scm_from_int(i);
	}

void init_butter() {
	radix10 = scm_from_int(10);
	scm_gc_protect_object(radix10);
	scm_c_define_gsubr("to-s", 1, 0, 0, to_s);
	scm_c_define_gsubr("to-i", 1, 0, 0, to_i);
	}
