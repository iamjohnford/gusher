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
#include <libpq-fe.h>
#include <string.h>
#include <math.h>

#include "gtime.h"
#include "log.h"

#define c2s(a) (scm_from_locale_string(a))

struct pg_conn {
	PGconn *conn;
	};

struct pg_res {
	PGresult *res;
	int cursor;
	int nfields;
	int tuples;
	SCM types;
	SCM fields;
	};

static scm_t_bits pg_conn_tag;
static scm_t_bits pg_res_tag;

static SCM pg_open(SCM conninfo) {
	SCM smob;
	struct pg_conn *pgc;
	char *conninfo_s;
	conninfo_s = scm_to_locale_string(conninfo);
	pgc = (struct pg_conn *)scm_gc_malloc(sizeof(struct pg_conn),
					"pg_conn");
	pgc->conn = PQconnectdb(conninfo_s);
	free(conninfo_s);
	if (PQstatus(pgc->conn) != CONNECTION_OK) {
		log_msg("PQ connection failed: %s\n",
				PQerrorMessage(pgc->conn));
		PQfinish(pgc->conn);
		pgc->conn = NULL;
		}
	SCM_NEWSMOB(smob, pg_conn_tag, pgc);
	return smob;
	}

static SCM pg_close(SCM conn) {
	struct pg_conn *pgc;
	scm_assert_smob_type(pg_conn_tag, conn);
	pgc = (struct pg_conn *)SCM_SMOB_DATA(conn);
	if (pgc->conn != NULL) PQfinish(pgc->conn);
	pgc->conn = NULL;
	return SCM_UNSPECIFIED;
	}

static SCM pg_exec(SCM conn, SCM query) {
	struct pg_conn *pgc;
	struct pg_res *pgr;
	char *query_s;
	int i;
	SCM res_smob;
	scm_assert_smob_type(pg_conn_tag, conn);
	pgc = (struct pg_conn *)SCM_SMOB_DATA(conn);
	pgr = (struct pg_res *)scm_gc_malloc(sizeof(struct pg_res),
					"pg_res");
	query_s = scm_to_locale_string(query);
	pgr->res = PQexec(pgc->conn, query_s);
	scm_remember_upto_here_1(conn);
	free(query_s);
	pgr->cursor = 0;
	pgr->fields = SCM_EOL;
	pgr->types = SCM_EOL;
	pgr->nfields = PQnfields(pgr->res);
	pgr->tuples = PQntuples(pgr->res);
	for (i = pgr->nfields - 1; i >= 0; i--) {
		pgr->fields = scm_cons(scm_from_utf8_symbol(
			PQfname(pgr->res, i)), pgr->fields);
		pgr->types = scm_cons(scm_from_unsigned_integer(PQftype(pgr->res, i)),
				pgr->types);
		}
	SCM_NEWSMOB(res_smob, pg_res_tag, pgr);
	return res_smob;
	}

static SCM decode_timestamp(const char *string) {
	const char *here;
	int year, month, day, hour, min, sec, msec;
	double fsec;
	here = string;
	year = atoi(here);
	here = index(here, '-') + 1;
	month = atoi(here);
	here = index(here, '-') + 1;
	day = atoi(here);
	if (index(here, ':') == NULL) hour = min = sec = msec = 0;
	else {
		here = index(here, ' ') + 1;
		hour = atoi(here);
		here = index(here, ':') + 1;
		min = atoi(here);
		here = index(here, ':') + 1;
		fsec = atof(here);
		sec = (int)fsec;
		msec = (int)((fsec - sec) * 1000 + 0.5);
		if (msec >= 1000) {
			sec += 1;
			msec -= 1000;
			}
		}
	return local_time_intern(year, month, day, hour, min, sec, msec);
	}

static SCM pg_decode(char *string, int dtype) {
	switch (dtype) {
		case 701:
		case 1700:
		case 700:
			return scm_from_double(atof(string));
		case 20:
		case 21:
		case 23:
			return scm_from_signed_integer(atoi(string));
		case 1114:
		case 1184:
		case 1082:
			return decode_timestamp(string);
		}
	return scm_from_utf8_string(string);
	}

static SCM build_row(struct pg_res *pgr) {
	SCM row, fnode, pair, value, tnode;
	int i;
	row = pair = value = SCM_EOL;
	fnode = pgr->fields;
	tnode = pgr->types;
	for (i = 0; i < pgr->nfields; i++) {
		if (PQgetisnull(pgr->res, pgr->cursor, i))
			value = SCM_EOL;
		else {
			value = pg_decode(PQgetvalue(pgr->res,
					pgr->cursor, i),
					scm_to_int(SCM_CAR(tnode)));
			}
		pair = scm_cons(SCM_CAR(fnode), value);
		row = scm_cons(pair, row);
		fnode = SCM_CDR(fnode);
		tnode = SCM_CDR(tnode);
		}
	scm_remember_upto_here_2(row, value);
	scm_remember_upto_here_1(pair);
	return row;
	}

static SCM pg_get_row(SCM res) {
	struct pg_res *pgr;
	SCM row;
	scm_assert_smob_type(pg_res_tag, res);
	pgr = (struct pg_res *)SCM_SMOB_DATA(res);
	if (pgr->cursor >= pgr->tuples) return SCM_BOOL_F;
	row = build_row(pgr);
	pgr->cursor++;
	scm_remember_upto_here_2(res, row);
	return row;
	}

static SCM pg_each_row(SCM res, SCM func) {
	struct pg_res *pgr;
	scm_assert_smob_type(pg_res_tag, res);
	pgr = (struct pg_res *)SCM_SMOB_DATA(res);
	while (pgr->cursor < pgr->tuples) {
		scm_call_1(func, build_row(pgr));
		pgr->cursor++;
		}
	PQclear(pgr->res);
	pgr->res = NULL;
	return SCM_UNSPECIFIED;
	}

static SCM pg_map_rows(SCM res, SCM rest) {
	struct pg_res *pgr;
	SCM bag, row;
	scm_assert_smob_type(pg_res_tag, res);
	bag = row = SCM_EOL;
	pgr = (struct pg_res *)SCM_SMOB_DATA(res);
	while (pgr->cursor < pgr->tuples) {
		row = build_row(pgr);
		if (!scm_is_null(rest))
			bag = scm_cons(scm_call_1(SCM_CAR(rest), row), bag);
		else bag = scm_cons(row, bag);
		pgr->cursor++;
		}
	PQclear(pgr->res);
	pgr->res = NULL;
	bag = scm_reverse(bag);
	scm_remember_upto_here_2(bag, row);
	return bag;
	}

static SCM pg_done(SCM res) {
	struct pg_res *pgr;
	scm_assert_smob_type(pg_res_tag, res);
	pgr = (struct pg_res *)SCM_SMOB_DATA(res);
	return (pgr->res == NULL ? SCM_BOOL_T : SCM_BOOL_F);
	}

static SCM pg_next_row(SCM res) {
	struct pg_res *pgr;
	SCM row;
	scm_assert_smob_type(pg_res_tag, res);
	pgr = (struct pg_res *)SCM_SMOB_DATA(res);
	if (pgr->cursor >= pgr->tuples) {
		PQclear(pgr->res);
		pgr->res = NULL;
		return SCM_BOOL_F;
		}
	row = build_row(pgr);
	pgr->cursor++;
	scm_remember_upto_here_1(row);
	return row;
	}

static SCM pg_clear(SCM res) {
	struct pg_res *pgr;
	scm_assert_smob_type(pg_res_tag, res);
	pgr = (struct pg_res *)SCM_SMOB_DATA(res);
	if (pgr->res != NULL) PQclear(pgr->res);
	pgr->res = NULL;
	return SCM_UNSPECIFIED;
	}

static SCM pg_fields(SCM res) {
	struct pg_res *pgr;
	scm_assert_smob_type(pg_res_tag, res);
	pgr = (struct pg_res *)SCM_SMOB_DATA(res);
	return pgr->fields;
	}

static SCM pg_tuples(SCM res) {
	struct pg_res *pgr;
	SCM out;
	scm_assert_smob_type(pg_res_tag, res);
	pgr = (struct pg_res *)SCM_SMOB_DATA(res);
	out = scm_from_signed_integer(pgr->tuples);
	scm_remember_upto_here_1(res);
	return out;
	}

static size_t free_pg_conn(SCM smob) {
	struct pg_conn *pgc;
	pgc = (struct pg_conn *)SCM_SMOB_DATA(smob);
	if (pgc->conn != NULL) PQfinish(pgc->conn);
	scm_gc_free(pgc, sizeof(struct pg_conn), "pg_conn");
	return 0;
	}

static size_t free_pg_res(SCM smob) {
	struct pg_res *pgr;
	pgr = (struct pg_res *)SCM_SMOB_DATA(smob);
	if (pgr->res != NULL) PQclear(pgr->res);
	scm_gc_free(pgr, sizeof(struct pg_res), "pg_res");
	return 0;
	}

static SCM mark_pg_res(SCM smob) {
	struct pg_res *pgr;
	pgr = (struct pg_res *)SCM_SMOB_DATA(smob);
	scm_gc_mark(pgr->fields);
	scm_gc_mark(pgr->types);
	return SCM_BOOL_F;
	}

static SCM pg_format_sql(SCM conn, SCM obj) {
	struct pg_conn *pgc;
	SCM out;
	if (SCM_SMOB_PREDICATE(time_tag, obj)) {
		out = format_time(obj, c2s("'%Y-%m-%d %H:%M:%S'"));
		}
	else if (scm_boolean_p(obj) == SCM_BOOL_T) {
		if (scm_is_true(obj)) out = c2s("'t'");
		else out = c2s("'f'");
		}
	else if (scm_is_number(obj)) {
		out = scm_number_to_string(obj,
			scm_from_signed_integer(10));
		}
	else if (scm_is_symbol(obj)) {
		out = pg_format_sql(conn, scm_symbol_to_string(obj));
		}
	else if (scm_is_string(obj)) {
		if (scm_string_null_p(obj) == SCM_BOOL_T) out = c2s("NULL");
		else {
			char *src = scm_to_locale_string(obj);
			scm_assert_smob_type(pg_conn_tag, conn);
			pgc = (struct pg_conn *)SCM_SMOB_DATA(conn);
			char *sql = PQescapeLiteral(pgc->conn,
					src, strlen(src));
			out = c2s(sql);
			free(src);
			PQfreemem(sql);
			}
		}
	else if (scm_is_null(obj)) out = c2s("NULL");
	else out = c2s("NULL");
	scm_remember_upto_here_1(out);
	return out;
	}

void init_postgres(void) {
	pg_conn_tag = scm_make_smob_type("pg_conn", sizeof(struct pg_conn));
	pg_res_tag = scm_make_smob_type("pg_res", sizeof(struct pg_res));
	scm_set_smob_free(pg_conn_tag, free_pg_conn);
	scm_set_smob_free(pg_res_tag, free_pg_res);
	scm_set_smob_mark(pg_res_tag, mark_pg_res);
	scm_c_define_gsubr("pg-open", 1, 0, 0, pg_open);
	scm_c_define_gsubr("pg-close", 1, 0, 0, pg_close);
	scm_c_define_gsubr("pg-exec", 2, 0, 0, pg_exec);
	scm_c_define_gsubr("pg-clear", 1, 0, 0, pg_clear);
	scm_c_define_gsubr("pg-tuples", 1, 0, 0, pg_tuples);
	scm_c_define_gsubr("pg-fields", 1, 0, 0, pg_fields);
	scm_c_define_gsubr("pg-get-row", 1, 0, 0, pg_get_row);
	scm_c_define_gsubr("pg-each-row", 2, 0, 0, pg_each_row);
	scm_c_define_gsubr("pg-next-row", 1, 0, 0, pg_next_row);
	scm_c_define_gsubr("pg-map-rows", 1, 0, 1, pg_map_rows);
	scm_c_define_gsubr("pg-end-stream?", 1, 0, 0, pg_done);
	scm_c_define_gsubr("pg-format", 2, 0, 0, pg_format_sql);
	}
	
