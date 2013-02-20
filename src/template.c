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

static SCM fill_template(SCM template, SCM slots) {
	char *master;
	SCM node, pair, parts;
	struct template_slot *table;
	char *pin, *sense, hold;
	int marklen, tabsize, i;
	master = scm_to_locale_string(template);
	pin = master;
	parts = SCM_EOL;
	marklen = strlen(SLOT_MARK);
	tabsize = scm_to_int(scm_length(slots));
	table = (struct template_slot *)malloc(
				sizeof(struct template_slot) * tabsize);
	i = 0;
	for (node = slots; node != SCM_EOL; node = SCM_CDR(node)) {
		pair = SCM_CAR(node);
		table[i].token = upcase(scm_to_locale_string(
				scm_symbol_to_string(SCM_CAR(pair))));
		table[i].payload = scm_to_locale_string(SCM_CDR(pair));
		i++;
		}
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
		pin = sense + marklen;
		}
	free(master);
	for (i = 0; i < tabsize; i++) {
		free(table[i].token);
		free(table[i].payload);
		}
	free(table);
	return scm_string_concatenate(scm_reverse(parts));
	}

void init_template(void) {
	scm_c_define_gsubr("fill-template", 2, 0, 0, fill_template);
	}
