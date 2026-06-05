#include <ctype.h>
#include "gwlib/gwlib.h"
#include "gwlib/dbpool.h"
#include "gw/smsc/smpp_pdu.h"
#include "smpp_database.h"
#include "smpp_pdu_util.h"
#include "smpp_database_util.h"

int smpp_db_sql_is_pgsql(SMPPDatabase *db)
{
    return db != NULL && db->sql_dialect == DBPOOL_PGSQL;
}

Octstr *smpp_db_sql_string_literal(Octstr *value)
{
    long i, len;
    Octstr *out;

    if (value == NULL)
        return octstr_create("NULL");
    out = octstr_create("'");
    len = octstr_len(value);
    for (i = 0; i < len; i++) {
        char c = octstr_get_char(value, i);
        if (c == '\'')
            octstr_append_cstr(out, "''");
        else
            octstr_append_char(out, c);
    }
    octstr_append_char(out, '\'');
    return out;
}

void smpp_db_sql_append_column(Octstr *sql, SMPPDatabase *db, const char *name)
{
    if (smpp_db_sql_is_pgsql(db))
        octstr_format_append(sql, "%s", name);
    else
        octstr_format_append(sql, "`%s`", name);
}

void smpp_db_sql_append_where_eq(Octstr *sql, List *binds, SMPPDatabase *db, const char *col, Octstr *val)
{
    smpp_db_sql_append_column(sql, db, col);
    if (smpp_db_sql_is_pgsql(db)) {
        Octstr *lit = smpp_db_sql_string_literal(val);
        octstr_format_append(sql, " = %S", lit);
        octstr_destroy(lit);
    } else {
        octstr_append_cstr(sql, " = ?");
        gwlist_produce(binds, octstr_duplicate(val));
    }
}

void smpp_db_sql_append_insert_value(Octstr *values, List *binds, SMPPDatabase *db, Octstr *val)
{
    if (smpp_db_sql_is_pgsql(db)) {
        Octstr *lit = smpp_db_sql_string_literal(val);
        octstr_format_append(values, "%S,", lit);
        octstr_destroy(lit);
    } else {
        gwlist_produce(binds, octstr_duplicate(val));
        octstr_append_cstr(values, "?,");
    }
}

void smpp_db_sql_append_insert_long(Octstr *values, List *binds, SMPPDatabase *db, long val)
{
    if (smpp_db_sql_is_pgsql(db)) {
        octstr_format_append(values, "%ld,", val);
    } else {
        gwlist_produce(binds, octstr_format("%ld", val));
        octstr_append_cstr(values, "?,");
    }
}

void smpp_db_sql_append_password_check(Octstr *sql, List *binds, SMPPDatabase *db, Octstr *password)
{
    if (smpp_db_sql_is_pgsql(db)) {
        Octstr *lit = smpp_db_sql_string_literal(password);
        octstr_format_append(sql, " AND password = encode(digest(%S, 'sha256'), 'hex')", lit);
        octstr_destroy(lit);
    } else {
        octstr_append_cstr(sql, " AND `password` = SHA2(?, 256)");
        gwlist_produce(binds, octstr_duplicate(password));
    }
}

const char *smpp_db_sql_union_keyword(SMPPDatabase *db)
{
    if (smpp_db_sql_is_pgsql(db))
        return "UNION";
    return "UNION DISTINCT";
}

static int smpp_db_hex_nibble(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

Octstr *smpp_db_url_encode_octstr(const Octstr *in)
{
    long i, len;
    Octstr *out = octstr_create("");

    if (in == NULL || (len = octstr_len(in)) == 0)
        return out;
    for (i = 0; i < len; i++) {
        int b = (unsigned char) octstr_get_char(in, i);
        octstr_format_append(out, "%%%02X", b);
    }
    return out;
}

Octstr *smpp_db_url_decode_octstr(const Octstr *s)
{
    long i, len;
    Octstr *out;

    if (s == NULL || (len = octstr_len(s)) == 0)
        return octstr_create("");
    if (octstr_search_char(s, '%', 0) < 0)
        return octstr_duplicate(s);

    out = octstr_create("");
    for (i = 0; i < len;) {
        if (i + 2 < len && octstr_get_char(s, i) == '%') {
            int hi = smpp_db_hex_nibble(octstr_get_char(s, i + 1));
            int lo = smpp_db_hex_nibble(octstr_get_char(s, i + 2));
            if (hi < 0 || lo < 0) {
                octstr_destroy(out);
                return octstr_duplicate(s);
            }
            octstr_append_char(out, (char) ((hi << 4) | lo));
            i += 3;
        } else {
            octstr_destroy(out);
            return octstr_duplicate(s);
        }
    }
    return out;
}

Octstr *smpp_pdu_pack_without_command_length(Octstr *smsc_id, SMPP_PDU *pdu)
{
    Octstr *os = smpp_pdu_pack(smsc_id, pdu);
    Octstr *result = octstr_copy(os, 4, octstr_len(os) - 4);
    octstr_destroy(os);
    if (result == NULL) {
        result = octstr_create("");
    }
    return result;
}

Octstr *smpp_database_format_dlr_date(Octstr *date_os)
{
    char smpp_date[16];
    time_t t;

    if (!date_os || !octstr_len(date_os)) {
        return octstr_create("000000000000");
    }
    t = smpp_time_to_c_time(octstr_get_cstr(date_os));
    if (t > 0) {
        struct tm tm_tmp = gw_localtime(t);
        gw_strftime(smpp_date, sizeof(smpp_date), "%y%m%d%H%M%S", &tm_tmp);
        return octstr_create(smpp_date);
    }
    return octstr_duplicate(date_os);
}
