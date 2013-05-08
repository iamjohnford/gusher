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
static SCM infix;
static SCM string_cat_proc;

static SCM to_s(SCM obj) {
	if (scm_is_string(obj)) return obj;
	if (scm_is_number(obj)) return scm_number_to_string(obj, radix10);
	if (scm_is_symbol(obj)) return scm_symbol_to_string(obj);
	if (scm_is_null(obj)) return scm_from_locale_string("");
	if (obj == SCM_BOOL_T) return scm_from_locale_string("true");
	if (obj == SCM_BOOL_F) return scm_from_locale_string("false");
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
	else if (scm_is_real(obj)) i = (int)scm_to_double(obj);
	else if (scm_is_integer(obj)) return obj;
	else if (obj == SCM_BOOL_T) i = 1;
	return scm_from_int(i);
	}

static SCM string_cat(SCM glue, SCM items) {
	SCM list, node, item, cat;
	list = SCM_EOL;
	node = items;
	item = SCM_EOL;
	while (node != SCM_EOL) {
		item = SCM_CAR(node);
		if (scm_is_string(item))
			list = scm_cons(item, list);
		else if (scm_is_number(item))
			list = scm_cons(scm_number_to_string(item, radix10), list);
		else if (scm_is_symbol(item))
			list = scm_cons(scm_symbol_to_string(item), list);
		else if (scm_list_p(item) == SCM_BOOL_T)
			list = scm_cons(scm_apply_1(string_cat_proc, glue, item), list);
		node = SCM_CDR(node);
		}
	cat = scm_string_join(scm_reverse(list), glue, infix);
	scm_remember_upto_here_2(list, cat);
	scm_remember_upto_here_2(glue, items);
	scm_remember_upto_here_2(node, item);
	return cat;
	}

void init_butter() {
	radix10 = scm_from_int(10);
	scm_gc_protect_object(radix10);
	infix = scm_from_utf8_symbol("infix");
	scm_gc_protect_object(infix);
	scm_c_define_gsubr("to-s", 1, 0, 0, to_s);
	scm_c_define_gsubr("to-i", 1, 0, 0, to_i);
	scm_c_define_gsubr("string-cat", 1, 0, 1, string_cat);
	string_cat_proc = scm_c_eval_string("string-cat");
	scm_gc_protect_object(string_cat_proc);
	}
