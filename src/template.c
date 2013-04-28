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

#include <stdlib.h>
#include <libguile.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define c2s(s) (scm_from_locale_string(s))
#define SLOT_MARK "[["
#define SLOT_END "]]"

struct template_slot {
	char *token;
	char *payload;
	};

static char *upcase(char *src) {
	char *pt;
	for (pt = src; *pt; pt++) *pt = toupper(*pt);
	return src;
	}

static SCM fill_template(SCM template, SCM partial, SCM slots) {
	SCM node, pair, parts, slot_mark, slot_end, payload;
	struct template_slot *table;
	char *pin, *sense, hold, *master;
	int marklen, tabsize, i;
	master = scm_to_locale_string(template);
	scm_remember_upto_here_1(template);
	pin = master;
	parts = SCM_EOL;
	slot_mark = SCM_EOL;
	slot_end = SCM_EOL;
	marklen = strlen(SLOT_MARK);
	tabsize = scm_to_int(scm_length(slots));
	table = (struct template_slot *)malloc(
				sizeof(struct template_slot) * tabsize);
	i = 0;
	pair = SCM_EOL;
	payload = SCM_EOL;
	for (node = slots; node != SCM_EOL; node = SCM_CDR(node)) {
		pair = SCM_CAR(node);
		table[i].token = upcase(scm_to_locale_string(
				scm_symbol_to_string(SCM_CAR(pair))));
		payload = SCM_CDR(pair);
		if (scm_is_number(payload))
			payload = scm_number_to_string(payload,
						scm_from_int(10));
		table[i].payload = scm_to_locale_string(payload);
		i++;
		}
	scm_remember_upto_here_2(node, pair);
	scm_remember_upto_here_2(slots, payload);
	while (1) {
		if ((sense = strstr(pin, SLOT_MARK)) == NULL) {
			parts = scm_cons(c2s(pin), parts);
			break;
			}
		hold = *sense;
		*sense = '\0';
		parts = scm_cons(c2s(pin), parts);
		*sense = hold;
		pin = sense;
		if ((sense = strstr(pin, SLOT_END)) == NULL) {
			parts = scm_cons(c2s(pin), parts);
			break;
			}
		pin += marklen;
		*sense = '\0';
		for (i = 0; i < tabsize; i++) {
			if (strcmp(pin, table[i].token) == 0) {
				parts = scm_cons(c2s(table[i].payload),
						parts);
				break;
				}
			}
		if ((i == tabsize) && (partial == SCM_BOOL_T)) {
			if (slot_mark == SCM_EOL) {
				slot_mark = c2s(SLOT_MARK);
				slot_end = c2s(SLOT_END);
				}
			parts = scm_cons(slot_mark, parts);
			parts = scm_cons(c2s(pin), parts);
			parts = scm_cons(slot_end, parts);
			}
		scm_remember_upto_here_1(partial);
		pin = sense + marklen;
		}
	free(master);
	for (i = 0; i < tabsize; i++) {
		free(table[i].token);
		free(table[i].payload);
		}
	free(table);
	SCM whole;
	whole = scm_string_concatenate(scm_reverse(parts));
	scm_remember_upto_here_2(parts, slot_mark);
	scm_remember_upto_here_1(slot_end);
	scm_remember_upto_here_1(whole);
	return whole;
	}

void init_template(void) {
	scm_c_define_gsubr("fill-template", 2, 0, 1, fill_template);
	}
