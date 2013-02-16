#include <libguile.h>
#include <libpq-fe.h>

struct pg_conn {
	PGconn *conn;
	};

static scm_t_bits pg_conn_tag;

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
		fprintf(stderr, "PQ connection failed: %s\n",
				PQerrorMessage(pgc->conn));
		PQfinish(pgc->conn);
		pgc->conn = NULL;
		}
	SCM_NEWSMOB(smob, pg_conn_tag, pgc);
	return smob;
	}

static SCM pg_close(SCM conn) {
	struct pg_conn *pgc;
	pgc = (struct pg_conn *)SCM_SMOB_DATA(conn);
	if (pgc->conn != NULL) PQfinish(pgc->conn);
	pgc->conn = NULL;
	return SCM_UNSPECIFIED;
	}

static size_t free_pg_conn(SCM smob) {
	struct pg_conn *pgc;
	pgc = (struct pg_conn *)SCM_SMOB_DATA(smob);
	if (pgc->conn != NULL) PQfinish(pgc->conn);
	scm_gc_free(pgc, sizeof(struct pg_conn), "pg_conn");
	return 0;
	}

void init_postgres(void) {
	pg_conn_tag = scm_make_smob_type("pg_conn", sizeof(struct pg_conn));
	scm_set_smob_free(pg_conn_tag, free_pg_conn);
	scm_c_define_gsubr("pg-open", 1, 0, 0, pg_open);
	scm_c_define_gsubr("pg-close", 1, 0, 0, pg_close);
	}
	
