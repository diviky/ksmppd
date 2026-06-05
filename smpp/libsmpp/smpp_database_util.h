#ifndef SMPP_DATABASE_UTIL_H
#define SMPP_DATABASE_UTIL_H

#include "gwlib/gwlib.h"
#include "gwlib/dbpool.h"
#include "gw/smsc/smpp_pdu.h"

struct SMPPDatabase;

#ifdef __cplusplus
extern "C" {
#endif

int smpp_db_sql_is_pgsql(struct SMPPDatabase *db);
#define SMPP_DB_PGSQL(db) smpp_db_sql_is_pgsql((db))
Octstr *smpp_db_sql_string_literal(Octstr *value);
void smpp_db_sql_append_column(Octstr *sql, struct SMPPDatabase *db, const char *name);
void smpp_db_sql_append_where_eq(Octstr *sql, List *binds, struct SMPPDatabase *db, const char *col, Octstr *val);
void smpp_db_sql_append_insert_value(Octstr *values, List *binds, struct SMPPDatabase *db, Octstr *val);
void smpp_db_sql_append_insert_long(Octstr *values, List *binds, struct SMPPDatabase *db, long val);
void smpp_db_sql_append_password_check(Octstr *sql, List *binds, struct SMPPDatabase *db, Octstr *password);
const char *smpp_db_sql_union_keyword(struct SMPPDatabase *db);

Octstr *smpp_db_url_encode_octstr(const Octstr *in);
Octstr *smpp_db_url_decode_octstr(const Octstr *s);
Octstr *smpp_pdu_pack_without_command_length(Octstr *smsc_id, SMPP_PDU *pdu);
Octstr *smpp_database_format_dlr_date(Octstr *date_os);

#ifdef __cplusplus
}
#endif

#endif /* SMPP_DATABASE_UTIL_H */
