/* ====================================================================
 * KSMPPD Software License, Version 1.0
 *
 * Copyright (c) 2016 Kurt Neo
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by
 *        Kurt Neo <kneodev@gmail.com> & the Kannel Group (http://www.kannel.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "KSMPPD" and "KSMPPD" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please
 *    contact kneodev@gmail.com
 *
 * 5. Products derived from this software may not be called "KSMPPD",
 *    nor may "KSMPPD" appear in their name, without prior written
 *    permission of the Kurt Neo.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL KURT NEO OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by Kurt Neo.
 *
 * KSMPPD or "Kurt's SMPP Daemon" were written by Kurt Neo.
 *
 * If you would like to donate to this project you may do so via Bitcoin to
 * the address: 1NhLkTDiZtFTJMefvjQY4pUWM3jD641jWN
 *
 * If you require commercial support for this software you can contact
 *
 * Kurt Neo <kneodev@gmail.com>
 *
 * This product includes software developed by the Kannel Group (http://www.kannel.org/).
 *
 */

#include <ctype.h>
#include <gw/sms.h>
#include "gwlib/gwlib.h"
#include "gwlib/dbpool.h"
#include "gw/msg.h"
#include "gw/load.h"
#include "gw/smsc/smpp_pdu.h"
#include "smpp_server.h"
#include "smpp_bearerbox.h"
#include "smpp_esme.h"
#include "smpp_queued_pdu.h"
#include "gw/dlr.h"
#include "smpp_database.h"
#include "smpp_route.h"
#include "smpp_pdu_util.h"
#include "smpp_database_util.h"

/*
 User table scheme:

 CREATE TABLE smpp_user (
 `system_id` varchar(15) NOT NULL,
 `password` varchar(64) NOT NULL,
 `throughput` double(10,5) NOT NULL DEFAULT '0',
 `default_smsc` varchar(64) DEFAULT NULL,
 `callback_url` varchar(255) DEFAULT NULL,
 `simulate` tinyint(1) NOT NULL DEFAULT '0',
 `simulate_dlr_fail` tinyint(1) not null default '0',
 `simulate_deliver_every` int unsigned not null,
 `simulate_permanent_failure_every` int unsigned not null,
 `simulate_temporary_failure_every` int unsigned not null,
 PRIMARY KEY(system_id)
 );

 Demo user insert:

 -- User with simulation
 INSERT INTO smpp_user
 (`system_id`, `password`, `throughput`, `default_smsc`, `callback_url`, `simulate`, `simulate_dlr_fail`, `simulate_deliver_every`, `simulate_permanent_failure_every`, `simulate_temporary_failure_every`) VALUES
 ('rimas', SHA2('simulate', 256), 10.0, NULL, NULL, 1, 0, 0, 0, 1);

-- Normal user

 INSERT INTO smpp_user
 (`system_id`, `password`, `throughput`, `default_smsc`, `callback_url`, `simulate`, `simulate_dlr_fail`, `simulate_deliver_every`, `simulate_permanent_failure_every`, `simulate_temporary_failure_every`) VALUES
 ('simulate', SHA2('simulate', 256), 10.0, NULL, NULL, 0, 0, 0, 0, 0);

 */

#include "smpp_database_redis.h"

static int smpp_database_mysql_create_store_table(DBPoolConn *conn, Octstr *table_name, Msg *msg);
static int smpp_database_pgsql_create_store_table(DBPoolConn *conn, Octstr *table_name, Msg *msg);
static int smpp_database_create_store_table(SMPPDatabase *smpp_database, DBPoolConn *conn, Octstr *table_name, Msg *msg);
static int smpp_database_sql_init_tables(SMPPServer *smpp_server, SMPPDatabase *smpp_database);
static SMPPDatabase *smpp_database_relational_setup(SMPPServer *smpp_server, DBPool *pool, enum db_type dialect);

int smpp_database_mysql_remove_stored_pdu(SMPPServer *smpp_server, Octstr *global_id) {
    SMPPDatabase *smpp_database = smpp_server->database;
    Octstr *sql;
    DBPool *pool = smpp_database->context;

    int res = 0;

    DBPoolConn *conn;

    sql = octstr_format("DELETE FROM %S WHERE global_id = %S", smpp_server->database_pdu_table, global_id);

    conn = dbpool_conn_consume(pool);

    if ((res = dbpool_conn_update(conn, sql, NULL)) == -1) {
        error(0, "Query error '%s'", octstr_get_cstr(sql));
        res = 0;
    } else {
        res = 1;
    }

    dbpool_conn_produce(conn);

    octstr_destroy(sql);

    return res;
}

void smpp_database_mysql_queued_pdu_handler(void *context, long status) {
    SMPPQueuedPDU *smpp_queued_pdu = context;
    if(!smpp_queued_pdu->smpp_server) {
        error(0, "No SMPP Server context, can't proceed");
        return;
    }

    debug("smpp.database.mysql.queued.pdu.handler", 0, "Got queued database callback for %s %s", octstr_get_cstr(smpp_queued_pdu->system_id), octstr_get_cstr(smpp_queued_pdu->bearerbox_id));

    SMPPDatabase *smpp_database = smpp_queued_pdu->smpp_server->database;

    if((status != SMPP_QUEUED_PDU_DESTROYED) && (status != SMPP_ESME_COMMAND_STATUS_WAIT_ACK_TIMEOUT)) {
        /* Don't try again */
        smpp_database_mysql_remove_stored_pdu(smpp_queued_pdu->smpp_server, smpp_queued_pdu->bearerbox_id);
    }

    dict_remove(smpp_database->pending_pdu, smpp_queued_pdu->bearerbox_id);
    smpp_queued_pdu_destroy(smpp_queued_pdu);
}

List *smpp_database_mysql_get_stored_pdu(SMPPServer *smpp_server,  Octstr *service, long limit) {
    SMPPDatabase *smpp_database = smpp_server->database;
    DBPool *pool = smpp_database->context;
    Octstr *sql;
    Octstr *tmp = NULL;
    List *messages = gwlist_create();
    List *results = NULL;
    List *row;

    List *binds = gwlist_create();

    SMPPQueuedPDU *smpp_queued_pdu;

    DBPoolConn *conn;

    sql = octstr_format("SELECT global_id, time, system_id, pdu FROM %S WHERE ", smpp_server->database_pdu_table);
    smpp_db_sql_append_where_eq(sql, binds, smpp_database, "system_id", service);
    octstr_append_cstr(sql, " ");

    List *pending = dict_keys(smpp_database->pending_pdu);
    Octstr *pending_ids = NULL;
    if(gwlist_len(pending) > 0) {
        pending_ids = octstr_create(" AND global_id NOT IN (");
        while((tmp = gwlist_consume(pending)) != NULL) {
            octstr_format_append(pending_ids, "%S,", tmp);
            octstr_destroy(tmp);
        }
        octstr_delete(pending_ids, (octstr_len(pending_ids) - 1), 1);
        octstr_format_append(pending_ids, ")");

        octstr_format_append(sql, "%S", pending_ids);
        octstr_destroy(pending_ids);
    }
    gwlist_destroy(pending, (void(*)(void *))octstr_destroy);

    octstr_format_append(sql, " LIMIT %ld ",limit);

    conn = dbpool_conn_consume(pool);

    if(dbpool_conn_select(conn, sql, SMPP_DB_PGSQL(smpp_database) ? NULL : binds, &results) == -1) {
        error(0, "Error with query %s", octstr_get_cstr(sql));
    }

    octstr_destroy(sql);

    dbpool_conn_produce(conn);

    if (gwlist_len(results) > 0) {
        while ((row = gwlist_extract_first(results)) != NULL) {
            tmp = octstr_duplicate(gwlist_get(row, 0));

            smpp_queued_pdu = smpp_queued_pdu_create();
            dict_put(smpp_database->pending_pdu, tmp, smpp_queued_pdu);

            smpp_queued_pdu->bearerbox_id = octstr_duplicate(gwlist_get(row, 0));
            smpp_queued_pdu->callback = smpp_database_mysql_queued_pdu_handler;
            smpp_queued_pdu->context = smpp_queued_pdu;
            smpp_queued_pdu->system_id = octstr_duplicate(service);
            {
                Octstr *pdu_os = smpp_db_url_decode_octstr(gwlist_get(row, 3));
                smpp_queued_pdu->pdu = smpp_pdu_unpack(service, pdu_os);
                octstr_destroy(pdu_os);
            }
            smpp_queued_pdu->smpp_server = smpp_server;



            gwlist_produce(messages, smpp_queued_pdu);

            gwlist_destroy(row, octstr_destroy_item);
            octstr_destroy(tmp);
        }
    } else {
        /* No messages queued */
    }

    gwlist_destroy(binds, NULL); /* We didn't copy */
    gwlist_destroy(results, NULL);

    return messages;
}

List *smpp_database_mysql_get_esmes_with_queued(SMPPServer *smpp_server) {
    SMPPDatabase *smpp_database = smpp_server->database;
    DBPool *pool = smpp_database->context;
    Octstr *sql, *system_id;

    List *esmes = gwlist_create();
    List *results = NULL;
    List *row;

    DBPoolConn *conn;

    /* Only include smpp_store rows for mo/report_mo - messages to deliver TO ESME.
     * Exclude mt_push: in database-store-primary mode these are outbound MT for external
     * pickup, not for delivery to ESME. When bearerbox is used, mt_push are for bearerbox. */
    if(octstr_len(smpp_server->database_dlr_table)) {
        sql = octstr_format("SELECT LOWER(system_id) FROM %S %s SELECT LOWER(service) FROM %S WHERE sms_type IN (%ld, %ld) %s SELECT LOWER(service) FROM %S",
            smpp_server->database_pdu_table, smpp_db_sql_union_keyword(smpp_database),
            smpp_server->database_store_table, (long)mo, (long)report_mo, smpp_db_sql_union_keyword(smpp_database),
            smpp_server->database_dlr_table);
    } else {
        sql = octstr_format("SELECT LOWER(system_id) FROM %S %s SELECT LOWER(service) FROM %S WHERE sms_type IN (%ld, %ld)",
            smpp_server->database_pdu_table, smpp_db_sql_union_keyword(smpp_database),
            smpp_server->database_store_table, (long)mo, (long)report_mo);
    }

    conn = dbpool_conn_consume(pool);

    dbpool_conn_select(conn, sql, NULL, &results);

    octstr_destroy(sql);

    dbpool_conn_produce(conn);

    if (gwlist_len(results) > 0) {
        while ((row = gwlist_extract_first(results)) != NULL) {
            system_id = gwlist_get(row, 0);
            debug("smpp.database.mysql.get.esmes.with.queued", 0, "ESME %s has queued messages in store ", octstr_get_cstr(system_id));
            gwlist_produce(esmes, system_id);
            gwlist_destroy(row, NULL);
        }
    }

    gwlist_destroy(results, NULL);

    return esmes;
}

List *smpp_database_mysql_get_routes(SMPPServer *smpp_server, int direction, Octstr *service) {
    SMPPDatabase *smpp_database = smpp_server->database;
    DBPool *pool = smpp_database->context;
    Octstr *sql;

    List *routes = gwlist_create();
    List *results = NULL;
    List *row;

    List *binds = gwlist_create();

    DBPoolConn *conn;

    sql = octstr_format("SELECT regex, cost, system_id, smsc_id, source_regex FROM %S WHERE direction = %d ", smpp_server->database_route_table, direction);

    if(octstr_len(service)) {
        octstr_append_cstr(sql, " AND ");
        smpp_db_sql_append_where_eq(sql, binds, smpp_database, "system_id", service);
    }

    octstr_format_append(sql, " ORDER BY priority DESC");

    conn = dbpool_conn_consume(pool);

    dbpool_conn_select(conn, sql, SMPP_DB_PGSQL(smpp_database) ? NULL : binds, &results);

    octstr_destroy(sql);

    dbpool_conn_produce(conn);
    SMPPRoute *smpp_route;

    if (gwlist_len(results) > 0) {
        while ((row = gwlist_extract_first(results)) != NULL) {
            smpp_route = smpp_route_create();
            octstr_parse_double(&smpp_route->cost, gwlist_get(row, 1), 0);
            smpp_route->direction = direction;

            smpp_route->system_id = octstr_duplicate(gwlist_get(row, 2));
            octstr_convert_range(smpp_route->system_id, 0, octstr_len(smpp_route->system_id), tolower); /* Normalize for our routes */

            smpp_route->regex = gw_regex_comp(gwlist_get(row, 0), REG_EXTENDED);
            smpp_route->smsc_id = octstr_duplicate(gwlist_get(row,3));

            if(octstr_len(gwlist_get(row, 4))) {
                smpp_route->source_regex = gw_regex_comp(gwlist_get(row, 4), REG_EXTENDED);
            }

            if(!smpp_route->source_regex) {
                debug("smpp.database.mysql.get.routes", 0, "No source-regex, allowing all toward %s", octstr_get_cstr(gwlist_get(row,0)));
            }

            if(!smpp_route->regex) {
                error(0, "Failed to compile regex %s, ignoring",octstr_get_cstr(gwlist_get(row, 0)));
            } else {
                debug("smpp.database.mysql.get.routes", 0, "Added route direction = %d <-> %s for %s from %s via %s", smpp_route->direction, octstr_get_cstr(gwlist_get(row,3)), octstr_get_cstr(gwlist_get(row,0)), octstr_get_cstr(gwlist_get(row, 4)), octstr_get_cstr(smpp_route->system_id));
                gwlist_produce(routes, smpp_route);
            }

            gwlist_destroy(row, octstr_destroy_item);
        }
    }

    gwlist_destroy(binds, NULL); /* We didn't copy */
    gwlist_destroy(results, NULL);

    return routes;
}

List *smpp_database_mysql_get_stored(SMPPServer *smpp_server, long sms_type, Octstr *service, long limit, int store_kind) {
    SMPPDatabase *smpp_database = smpp_server->database;
    DBPool *pool = smpp_database->context;
    Octstr *sql;
    Octstr *store_table;
    Octstr *tmp = NULL;
    List *messages = gwlist_create();
    List *results = NULL;
    List *row;

    List *binds = NULL;

    Msg *msg = msg_create(sms);
    SMPPDatabaseMsg *smpp_database_msg;

    long position = 0;

    DBPoolConn *conn;

    char id[UUID_STR_LEN + 1];

    store_table = (store_kind >= 0)
            ? smpp_database_store_table_name(smpp_server, store_kind)
            : smpp_database_get_stored_table_name(smpp_server, sms_type);

    sql = octstr_format("SELECT global_id, ");

#define INTEGER(name) octstr_append_cstr(sql, "`" #name "`,"); if(p->name) { }
#define OCTSTR(name)  octstr_append_cstr(sql, "`" #name "`,");
#define UUID(name) uuid_unparse(p->name, id); \
                octstr_append_cstr(sql, "`" #name "`,");
#define VOID(name) ;
#define MSG(type, stmt) \
        case type: {struct type *p = &msg->type; stmt} break;
    switch (msg->type) {
#include "gw/msg-decl.h"
        default:
            msg_destroy(msg);
            return NULL;
    }


    msg_destroy(msg);

    octstr_delete(sql, (octstr_len(sql) - 1), 1);
    octstr_format_append(sql, " FROM %S WHERE sms_type = %ld ", store_table, sms_type);

    if(octstr_len(service)) {
        binds = gwlist_create();
        octstr_append_cstr(sql, " AND ");
        smpp_db_sql_append_where_eq(sql, binds, smpp_database, "service", service);
    }

    List *pending = dict_keys(smpp_database->pending_msg);
    Octstr *pending_ids = NULL;
    if(gwlist_len(pending) > 0) {
        debug("smpp.database.mysql.get.stored", 0, "Excluding in process number %ld (last one = %s)",gwlist_len(pending), octstr_get_cstr(gwlist_get(pending, (gwlist_len(pending)-1))));

        pending_ids = octstr_create(" AND global_id NOT IN (");
        while((tmp = gwlist_consume(pending)) != NULL) {
            octstr_format_append(pending_ids, "%S,", tmp);
            octstr_destroy(tmp);
        }
        octstr_delete(pending_ids, (octstr_len(pending_ids) - 1), 1);
        octstr_format_append(pending_ids, ")");

        octstr_format_append(sql, "%S", pending_ids);



        octstr_destroy(pending_ids);
    }
    gwlist_destroy(pending, (void(*)(void *))octstr_destroy);

    if(!limit) {
        limit = SMPP_DATABASE_BATCH_LIMIT;
    }

    octstr_format_append(sql, " LIMIT %ld", SMPP_DATABASE_BATCH_LIMIT);

    conn = dbpool_conn_consume(pool);

    dbpool_conn_select(conn, sql, SMPP_DB_PGSQL(smpp_database) ? NULL : binds, &results);

    octstr_destroy(sql);

    dbpool_conn_produce(conn);

    if (gwlist_len(results) > 0) {
        while ((row = gwlist_extract_first(results)) != NULL) {
            smpp_database_msg = smpp_database_msg_create();
            smpp_database_msg->global_id = atol(octstr_get_cstr(gwlist_get(row,0)));
            msg = msg_create(sms);
#define INTEGER(name) p->name = atol(octstr_get_cstr(gwlist_get(row,position))); ++position;
#define OCTSTR(name)  if(octstr_len(gwlist_get(row,position))) { p->name = octstr_duplicate(gwlist_get(row,position)); } ++position;
#define UUID(name) uuid_parse(gwlist_get(row, position), p->name); ++position;
#define VOID(name) ;
#define MSG(type, stmt) \
        case type: { struct type *p = &msg->type; position = 1; stmt; } break;
            switch (msg->type) {
#include "gw/msg-decl.h"
                default:
                    return NULL;
            }

            if (msg->sms.udhdata != NULL && octstr_len(msg->sms.udhdata) > 0) {
                Octstr *d = smpp_db_url_decode_octstr(msg->sms.udhdata);
                octstr_destroy(msg->sms.udhdata);
                msg->sms.udhdata = d;
            }
            if (msg->sms.msgdata != NULL && octstr_len(msg->sms.msgdata) > 0) {
                Octstr *d = smpp_db_url_decode_octstr(msg->sms.msgdata);
                octstr_destroy(msg->sms.msgdata);
                msg->sms.msgdata = d;
            }

            if(msg->sms.msgdata == NULL) {
                msg->sms.msgdata = octstr_create("");
            }

            smpp_database_msg->msg = msg;
            smpp_database_msg->smpp_server = smpp_server;
            smpp_database_msg->store_table = octstr_duplicate(store_table);

            debug("smpp.database.mysql.get.stored", 0, "Adding pending msg %s", octstr_get_cstr(gwlist_get(row, 0)));
            dict_put(smpp_database->pending_msg, gwlist_get(row, 0), smpp_database_msg);
            dict_put(smpp_database->pending_msg_store, gwlist_get(row, 0), octstr_duplicate(store_table));

            gwlist_produce(messages, smpp_database_msg);

            gwlist_destroy(row, octstr_destroy_item);
        }
    }

    gwlist_destroy(binds, NULL); /* We didn't copy */
    gwlist_destroy(results, NULL);
    octstr_destroy(store_table);

    return messages;
}

List *smpp_database_mysql_get_dlrs(SMPPServer *smpp_server, Octstr *service, long limit) {
    SMPPDatabase *smpp_database = smpp_server->database;
    if(!octstr_len(smpp_server->database_dlr_table)) {
        return gwlist_create();
    }
    DBPool *pool = smpp_database->context;
    Octstr *sql;
    List *messages = gwlist_create();
    List *results = NULL;
    List *row;
    List *binds = gwlist_create();
    SMPPDatabaseMsg *smpp_database_msg;
    Msg *msg;
    DBPoolConn *conn;
    long dlr_mask;
    Octstr *msgdata;
    Octstr *dlr_url;
    Octstr *submit_date_os;
    Octstr *done_date_os;
    Octstr *text_os;
    time_t submit_time;
    time_t done_time;

    if(!limit) {
        limit = SMPP_DATABASE_BATCH_LIMIT;
    }

    sql = octstr_format("SELECT global_id, message_id, service, status, err_code, submit_date, done_date, "
                       "destination_addr, source_addr, smsc_id, text FROM %S WHERE ",
                       smpp_server->database_dlr_table);
    if (SMPP_DB_PGSQL(smpp_database)) {
        Octstr *svc_lit = smpp_db_sql_string_literal(service);
        octstr_format_append(sql, "LOWER(service) = LOWER(%S) AND processed = 0 LIMIT %ld", svc_lit, limit);
        octstr_destroy(svc_lit);
    } else {
        octstr_format_append(sql, "LOWER(service) = LOWER(?) AND processed = 0 LIMIT %ld", limit);
        gwlist_produce(binds, octstr_duplicate(service));
    }

    conn = dbpool_conn_consume(pool);
    dbpool_conn_select(conn, sql, SMPP_DB_PGSQL(smpp_database) ? NULL : binds, &results);
    dbpool_conn_produce(conn);

    octstr_destroy(sql);
    gwlist_destroy(binds, octstr_destroy_item);

    debug("smpp.database.mysql.get.dlrs", 0, "Fetched %ld DLRs for service %s from %s",
          (long)gwlist_len(results), octstr_get_cstr(service), octstr_get_cstr(smpp_server->database_dlr_table));

    if (gwlist_len(results) > 0) {
        while ((row = gwlist_extract_first(results)) != NULL) {
            smpp_database_msg = smpp_database_msg_create();
            smpp_database_msg->global_id = atol(octstr_get_cstr(gwlist_get(row, 0)));
            smpp_database_msg->smpp_server = smpp_server;

            msg = msg_create(sms);
            msg->sms.sms_type = report_mo;
            msg->sms.service = octstr_duplicate(service);
            msg->sms.receiver = octstr_duplicate(gwlist_get(row, 7) ? gwlist_get(row, 7) : octstr_imm(""));
            msg->sms.sender = octstr_duplicate(gwlist_get(row, 8) ? gwlist_get(row, 8) : octstr_imm(""));
            msg->sms.smsc_id = octstr_duplicate(gwlist_get(row, 9) ? gwlist_get(row, 9) : octstr_imm(""));
            submit_date_os = smpp_database_format_dlr_date(gwlist_get(row, 5));
            done_date_os = smpp_database_format_dlr_date(gwlist_get(row, 6));
            text_os = gwlist_get(row, 10);
            if(!text_os) text_os = octstr_imm("");

            submit_time = smpp_time_to_c_time(octstr_get_cstr(submit_date_os));
            done_time = smpp_time_to_c_time(octstr_get_cstr(done_date_os));
            if (submit_time > 0) {
                msg->sms.time = submit_time;
            } else if (done_time > 0) {
                msg->sms.time = done_time;
            } else {
                msg->sms.time = time(NULL);
            }

            if(gwlist_get(row, 3) && octstr_case_compare(gwlist_get(row, 3), octstr_imm("DELIVRD")) == 0) {
                dlr_mask = DLR_SUCCESS;
            } else {
                dlr_mask = DLR_FAIL;
            }
            msg->sms.dlr_mask = dlr_mask;

            {
                Octstr *stat_os = gwlist_get(row, 3);
                Octstr *err_os = gwlist_get(row, 4);
                Octstr *dlvrd_str = (dlr_mask == DLR_SUCCESS) ? octstr_imm("001") : octstr_imm("000");
                unsigned int err_val = 0;
                if(err_os && octstr_len(err_os)) {
                    long tmp;
                    octstr_parse_long(&tmp, err_os, 0, 10);
                    err_val = (unsigned int)tmp;
                }
                msgdata = octstr_format("id:%S sub:001 dlvrd:%S submit date:%S done date:%S stat:%S err:%03x text:%S",
                    gwlist_get(row, 1),
                    dlvrd_str,
                    submit_date_os,
                    done_date_os,
                    stat_os && octstr_len(stat_os) ? stat_os : octstr_imm("DELIVRD"),
                    err_val,
                    text_os);
            }
            msg->sms.msgdata = msgdata;

            dlr_url = octstr_format("%S|%ld|%S", service, (long)msg->sms.time, gwlist_get(row, 1));
            msg->sms.dlr_url = dlr_url;

            smpp_database_msg->msg = msg;
            gwlist_produce(messages, smpp_database_msg);
            octstr_destroy(submit_date_os);
            octstr_destroy(done_date_os);
            gwlist_destroy(row, octstr_destroy_item);
        }
    }
    gwlist_destroy(results, NULL);

    return messages;
}

int smpp_database_mysql_remove_dlr(SMPPServer *smpp_server, unsigned long global_id) {
    SMPPDatabase *smpp_database = smpp_server->database;
    if(!octstr_len(smpp_server->database_dlr_table)) {
        return 0;
    }
    DBPool *pool = smpp_database->context;
    Octstr *sql;
    DBPoolConn *conn;
    int res = 0;

    sql = octstr_format("UPDATE %S SET processed = 1 WHERE global_id = %lu", smpp_server->database_dlr_table, global_id);
    conn = dbpool_conn_consume(pool);
    if (dbpool_conn_update(conn, sql, NULL) >= 0) {
        res = 1;
    }
    dbpool_conn_produce(conn);
    octstr_destroy(sql);
    return res;
}

static int smpp_database_mysql_create_store_table(DBPoolConn *conn, Octstr *table_name, Msg *msg)
{
    Octstr *sql;
    char id[UUID_STR_LEN + 1];
    int res;

    sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( global_id bigint unsigned not null auto_increment primary key, ", table_name);

#define INTEGER(name) octstr_append_cstr(sql, "`" #name "` bigint not null,");  if(p->name) { }
#define OCTSTR(name)  octstr_append_cstr(sql, "`" #name "` text default null,");
#define UUID(name) uuid_unparse(p->name, id); \
                octstr_append_cstr(sql, "`" #name "` varchar(128) default null,");
#define VOID(name) ;
#define MSG(type, stmt) \
        case type: {struct type *p = &msg->type; stmt} break;
    switch (msg->type) {
#include "gw/msg-decl.h"
        default:
            octstr_destroy(sql);
            return 0;
    }

    octstr_append_cstr(sql, "KEY `service` (`service`(16)),");
    octstr_append_cstr(sql, "KEY `sms_type` (`sms_type`)) ;");

    if ((res = dbpool_conn_update(conn, sql, NULL)) == -1) {
        error(0, "Query error '%s'", octstr_get_cstr(sql));
        res = 0;
    } else {
        res = 1;
    }
    octstr_destroy(sql);
    return res;
}

static int smpp_database_pgsql_create_store_table(DBPoolConn *conn, Octstr *table_name, Msg *msg)
{
    Octstr *sql;
    char id[UUID_STR_LEN + 1];
    int res;

    sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( global_id BIGSERIAL PRIMARY KEY, ", table_name);

#define INTEGER(name) octstr_append_cstr(sql, #name " BIGINT NOT NULL,");  if(p->name) { }
#define OCTSTR(name)  octstr_append_cstr(sql, #name " TEXT,");
#define UUID(name) uuid_unparse(p->name, id); \
                octstr_append_cstr(sql, #name " VARCHAR(128),");
#define VOID(name) ;
#define MSG(type, stmt) \
        case type: {struct type *p = &msg->type; stmt} break;
    switch (msg->type) {
#include "gw/msg-decl.h"
        default:
            octstr_destroy(sql);
            return 0;
    }

    octstr_delete(sql, (octstr_len(sql) - 1), 1);
    octstr_append_cstr(sql, ");");

    if ((res = dbpool_conn_update(conn, sql, NULL)) == -1) {
        error(0, "Query error '%s'", octstr_get_cstr(sql));
        octstr_destroy(sql);
        return 0;
    }
    octstr_destroy(sql);

    sql = octstr_format("CREATE INDEX IF NOT EXISTS %S_service_idx ON %S (service)", table_name, table_name);
    dbpool_conn_update(conn, sql, NULL);
    octstr_destroy(sql);
    sql = octstr_format("CREATE INDEX IF NOT EXISTS %S_sms_type_idx ON %S (sms_type)", table_name, table_name);
    res = dbpool_conn_update(conn, sql, NULL);
    octstr_destroy(sql);
    return res == -1 ? 0 : 1;
}

static int smpp_database_create_store_table(SMPPDatabase *smpp_database, DBPoolConn *conn, Octstr *table_name, Msg *msg)
{
    if (SMPP_DB_PGSQL(smpp_database))
        return smpp_database_pgsql_create_store_table(conn, table_name, msg);
    return smpp_database_mysql_create_store_table(conn, table_name, msg);
}

static int smpp_database_mysql_init_tables(SMPPServer *smpp_server, SMPPDatabase *smpp_database) {
    Octstr *sql;
    DBPool *pool = smpp_database->context;
    int skip_queue_tables = 0;

    Msg *msg = msg_create(sms);

    int res = 0;

    long running_version = 1;
    long our_version = 0;

    DBPoolConn *conn;

    List *binds = NULL;
    List *rows = NULL;

    char id[UUID_STR_LEN + 1];

    if (octstr_len(smpp_server->database_queue_type)
            && octstr_case_compare(smpp_server->database_queue_type, octstr_imm("redis")) == 0) {
        skip_queue_tables = 1;
        info(0, "Skipping MySQL queue table creation (Redis queue backend active)");
    }

    conn = dbpool_conn_consume(pool);

    if (!skip_queue_tables) {
        res = smpp_database_create_store_table(smpp_database, conn, smpp_server->database_store_table, msg);
        if (res && octstr_compare(smpp_server->database_store_table,
                smpp_server->database_queue_store_table) != 0) {
            res = smpp_database_create_store_table(smpp_database, conn, smpp_server->database_queue_store_table, msg);
        }

    if(res) {
        res = 0;
        sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( "
                "`global_id` bigint unsigned not null auto_increment primary key, "
                " `system_id` varchar(64) not null,"
                "`time` bigint, "
                "`pdu` blob,"
                " KEY `system_id` (`system_id`));", smpp_server->database_pdu_table);
        if ((res = dbpool_conn_update(conn, sql, NULL)) == -1) {
            error(0, "Query error '%s'", octstr_get_cstr(sql));
            res = 0;
        } else {
            res = 1;
        }
    }

    octstr_destroy(sql);

    if(octstr_len(smpp_server->database_dlr_table)) {
        sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( "
            "`global_id` bigint unsigned not null auto_increment primary key, "
            "`message_id` varchar(128) not null, "
            "`service` varchar(64) not null, "
            "`status` varchar(16) default 'DELIVRD', "
            "`err_code` int default 0, "
            "`submit_date` varchar(20) default null, "
            "`done_date` varchar(20) default null, "
            "`destination_addr` varchar(32) default null, "
            "`source_addr` varchar(32) default null, "
            "`smsc_id` varchar(64) default null, "
            "`text` text default null, "
            "`processed` tinyint default 0, "
            "KEY `service_processed` (`service`(16), `processed`), "
            "KEY `message_id` (`message_id`(32)));", smpp_server->database_dlr_table);
        if (dbpool_conn_update(conn, sql, NULL) == -1) {
            error(0, "Query error creating smpp_dlr table '%s'", octstr_get_cstr(sql));
        }
        octstr_destroy(sql);
    }
    } /* !skip_queue_tables */

    sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( "
            "`route_id` bigint unsigned not null auto_increment primary key,"
            "`direction` int not null,"
            "`regex` text, "
            "`cost` double,"
            "`system_id` varchar(64), "
            "`smsc_id` varchar(64), "
            " KEY `direction` (`direction`),"
            " KEY `system_id` (`system_id`),"
            " KEY `smsc_id` (`smsc_id`));", smpp_server->database_route_table);
    if ((res = dbpool_conn_update(conn, sql, NULL)) == -1) {
        error(0, "Query error '%s'", octstr_get_cstr(sql));
        res = 0;
    } else {
        res = 1;
    }

    octstr_destroy(sql);

    sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( "
      "`system_id` varchar(15) NOT NULL, "
      "`password` varchar(64) NOT NULL, "
      "`throughput` double(10,5) NOT NULL DEFAULT '0.00000',"
      "`default_smsc` varchar(64) DEFAULT NULL,"
      "`default_cost` double NOT NULL,"
      "`enable_prepaid_billing` int(10) unsigned NOT NULL DEFAULT '0',"
      "`credit` double NOT NULL DEFAULT '0',"
      "`callback_url` varchar(255) DEFAULT NULL,"
      "`simulate` tinyint(1) NOT NULL DEFAULT '0',"
      "`simulate_dlr_fail` tinyint(1) NOT NULL DEFAULT '0',"
      "`simulate_deliver_every` int(10) unsigned NOT NULL,"
      "`simulate_permanent_failure_every` int(10) unsigned NOT NULL,"
      "`simulate_temporary_failure_every` int(10) unsigned NOT NULL,"
      "`simulate_mo_every` int(10) unsigned NOT NULL,"
      "`max_binds` int(10) unsigned NOT NULL DEFAULT '0',"
      "`connect_allow_ip` text,"
      "PRIMARY KEY (`system_id`)"
      ");", smpp_server->database_user_table);
    if ((res = dbpool_conn_update(conn, sql, NULL)) == -1) {
        error(0, "Query error '%s'", octstr_get_cstr(sql));
        res = 0;
    } else {
        res = 1;
    }

    octstr_destroy(sql);
    msg_destroy(msg);


    sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( "
            "`component` varchar(54) not null, "
            "`version` int unsigned not null,"
            " PRIMARY KEY(`component`)"
            ");", smpp_server->database_version_table);


    if((res = dbpool_conn_update(conn, sql, NULL)) == -1) {
        error(0, "Query error '%s'", octstr_get_cstr(sql));
    } else {
        octstr_destroy(sql);
        sql = octstr_format("SELECT `version` FROM %S WHERE `component` = ?", smpp_server->database_version_table);
        binds = gwlist_create();
        gwlist_produce(binds, octstr_create("ksmppd"));

        if((res = dbpool_conn_select(conn, sql, binds, &rows)) == 0) {
            if(gwlist_len(rows) > 0) {
                running_version = atol(octstr_get_cstr(gwlist_get(gwlist_get(rows, 0), 0)));
                gwlist_destroy(gwlist_get(rows, 0), (void(*)(void *))octstr_destroy);
            } else {
                octstr_destroy(sql);
                sql = octstr_format("INSERT INTO %S (`component`, `version`) VALUES (?, %ld);", smpp_server->database_version_table, running_version);
                dbpool_conn_update(conn, sql, binds);
            }
            gwlist_destroy(rows, NULL);
        }

        debug("smpp.database.mysql.init.tables", 0, "Running database schema version %ld ", running_version);

        our_version = 2;
        if(running_version < our_version) {
            octstr_destroy(sql);
            sql = octstr_format("ALTER TABLE %S ADD COLUMN connect_allow_ip text", smpp_server->database_user_table);
            dbpool_conn_update(conn, sql, NULL);
            running_version = our_version;
        }

        our_version = 3;
        if(running_version < our_version) {
            octstr_destroy(sql);
            sql = octstr_format("ALTER TABLE %S ADD COLUMN source_regex text", smpp_server->database_route_table);
            dbpool_conn_update(conn, sql, NULL);
            running_version = our_version;
        }

        our_version = 4;
        if(running_version < our_version) {
            octstr_destroy(sql);
            sql = octstr_format("ALTER TABLE %S ADD COLUMN priority int DEFAULT '0'", smpp_server->database_route_table);
            dbpool_conn_update(conn, sql, NULL);
            running_version = our_version;
        }

        our_version = 5;
        if(running_version < our_version) {
            octstr_destroy(sql);
            sql = octstr_format("ALTER TABLE %S ADD COLUMN simulate_dlr_fail tinyint(1) NOT NULL DEFAULT '0'", smpp_server->database_user_table);
            dbpool_conn_update(conn, sql, NULL);
            running_version = our_version;
        }

        octstr_destroy(sql);
        sql = octstr_format("UPDATE %S SET `version` = %ld WHERE `component` = ?", smpp_server->database_version_table, running_version);
        dbpool_conn_update(conn, sql, binds);

        gwlist_destroy(binds, (void(*)(void *))octstr_destroy);
    }


    octstr_destroy(sql);
    dbpool_conn_produce(conn);





    return res;

}

static int smpp_database_pgsql_init_tables(SMPPServer *smpp_server, SMPPDatabase *smpp_database)
{
    Octstr *sql;
    DBPool *pool = smpp_database->context;
    int skip_queue_tables = 0;
    Msg *msg = msg_create(sms);
    int res = 0;
    long running_version = 1;
    long our_version = 0;
    DBPoolConn *conn;
    List *rows = NULL;

    if (octstr_len(smpp_server->database_queue_type)
            && octstr_case_compare(smpp_server->database_queue_type, octstr_imm("redis")) == 0) {
        skip_queue_tables = 1;
        info(0, "Skipping PostgreSQL queue table creation (Redis queue backend active)");
    }

    conn = dbpool_conn_consume(pool);
    dbpool_conn_update(conn, octstr_imm("CREATE EXTENSION IF NOT EXISTS pgcrypto"), NULL);

    if (!skip_queue_tables) {
        res = smpp_database_create_store_table(smpp_database, conn, smpp_server->database_store_table, msg);
        if (res && octstr_compare(smpp_server->database_store_table,
                smpp_server->database_queue_store_table) != 0) {
            res = smpp_database_create_store_table(smpp_database, conn, smpp_server->database_queue_store_table, msg);
        }

        if (res) {
            sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( "
                    "global_id BIGSERIAL PRIMARY KEY, "
                    "system_id VARCHAR(64) NOT NULL, "
                    "time BIGINT, "
                    "pdu BYTEA)", smpp_server->database_pdu_table);
            res = dbpool_conn_update(conn, sql, NULL) == -1 ? 0 : 1;
            octstr_destroy(sql);
            if (res) {
                sql = octstr_format("CREATE INDEX IF NOT EXISTS %S_system_id_idx ON %S (system_id)",
                        smpp_server->database_pdu_table, smpp_server->database_pdu_table);
                dbpool_conn_update(conn, sql, NULL);
                octstr_destroy(sql);
            }
        }

        if (octstr_len(smpp_server->database_dlr_table)) {
            sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( "
                "global_id BIGSERIAL PRIMARY KEY, "
                "message_id VARCHAR(128) NOT NULL, "
                "service VARCHAR(64) NOT NULL, "
                "status VARCHAR(16) DEFAULT 'DELIVRD', "
                "err_code INT DEFAULT 0, "
                "submit_date VARCHAR(20), "
                "done_date VARCHAR(20), "
                "destination_addr VARCHAR(32), "
                "source_addr VARCHAR(32), "
                "smsc_id VARCHAR(64), "
                "text TEXT, "
                "processed SMALLINT DEFAULT 0)", smpp_server->database_dlr_table);
            if (dbpool_conn_update(conn, sql, NULL) == -1) {
                error(0, "Query error creating smpp_dlr table '%s'", octstr_get_cstr(sql));
            }
            octstr_destroy(sql);
            sql = octstr_format("CREATE INDEX IF NOT EXISTS %S_service_processed_idx ON %S (service, processed)",
                    smpp_server->database_dlr_table, smpp_server->database_dlr_table);
            dbpool_conn_update(conn, sql, NULL);
            octstr_destroy(sql);
            sql = octstr_format("CREATE INDEX IF NOT EXISTS %S_message_id_idx ON %S (message_id)",
                    smpp_server->database_dlr_table, smpp_server->database_dlr_table);
            dbpool_conn_update(conn, sql, NULL);
            octstr_destroy(sql);
        }
    }

    sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( "
            "route_id BIGSERIAL PRIMARY KEY, "
            "direction INT NOT NULL, "
            "regex TEXT, "
            "cost DOUBLE PRECISION, "
            "system_id VARCHAR(64), "
            "smsc_id VARCHAR(64), "
            "source_regex TEXT, "
            "priority INT DEFAULT 0)", smpp_server->database_route_table);
    if ((res = dbpool_conn_update(conn, sql, NULL)) == -1) {
        error(0, "Query error '%s'", octstr_get_cstr(sql));
        res = 0;
    } else {
        res = 1;
    }
    octstr_destroy(sql);

    sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( "
            "system_id VARCHAR(15) NOT NULL PRIMARY KEY, "
            "password VARCHAR(64) NOT NULL, "
            "throughput DOUBLE PRECISION NOT NULL DEFAULT 0, "
            "default_smsc VARCHAR(64), "
            "default_cost DOUBLE PRECISION NOT NULL, "
            "enable_prepaid_billing INT NOT NULL DEFAULT 0, "
            "credit DOUBLE PRECISION NOT NULL DEFAULT 0, "
            "callback_url VARCHAR(255), "
            "simulate SMALLINT NOT NULL DEFAULT 0, "
            "simulate_dlr_fail SMALLINT NOT NULL DEFAULT 0, "
            "simulate_deliver_every INT NOT NULL, "
            "simulate_permanent_failure_every INT NOT NULL, "
            "simulate_temporary_failure_every INT NOT NULL, "
            "simulate_mo_every INT NOT NULL, "
            "max_binds INT NOT NULL DEFAULT 0, "
            "connect_allow_ip TEXT)", smpp_server->database_user_table);
    if ((res = dbpool_conn_update(conn, sql, NULL)) == -1) {
        error(0, "Query error '%s'", octstr_get_cstr(sql));
        res = 0;
    }
    octstr_destroy(sql);
    msg_destroy(msg);

    sql = octstr_format("CREATE TABLE IF NOT EXISTS %S ( "
            "component VARCHAR(54) NOT NULL PRIMARY KEY, "
            "version INT NOT NULL)", smpp_server->database_version_table);
    if (dbpool_conn_update(conn, sql, NULL) == -1) {
        error(0, "Query error '%s'", octstr_get_cstr(sql));
    } else {
        octstr_destroy(sql);
        sql = octstr_format("SELECT version FROM %S WHERE component = 'ksmppd'", smpp_server->database_version_table);
        if (dbpool_conn_select(conn, sql, NULL, &rows) == 0) {
            if (gwlist_len(rows) > 0) {
                running_version = atol(octstr_get_cstr(gwlist_get(gwlist_get(rows, 0), 0)));
                gwlist_destroy(gwlist_get(rows, 0), (void(*)(void *))octstr_destroy);
            } else {
                octstr_destroy(sql);
                sql = octstr_format("INSERT INTO %S (component, version) VALUES ('ksmppd', %ld)",
                        smpp_server->database_version_table, running_version);
                dbpool_conn_update(conn, sql, NULL);
            }
            gwlist_destroy(rows, NULL);
        }
        debug("smpp.database.pgsql.init.tables", 0, "Running database schema version %ld ", running_version);

        our_version = 5;
        if (running_version < our_version) {
            running_version = our_version;
            sql = octstr_format("UPDATE %S SET version = %ld WHERE component = 'ksmppd'",
                    smpp_server->database_version_table, running_version);
            dbpool_conn_update(conn, sql, NULL);
        }
        octstr_destroy(sql);
    }

    dbpool_conn_produce(conn);
    return res;
}

static int smpp_database_sql_init_tables(SMPPServer *smpp_server, SMPPDatabase *smpp_database)
{
    if (SMPP_DB_PGSQL(smpp_database))
        return smpp_database_pgsql_init_tables(smpp_server, smpp_database);
    return smpp_database_mysql_init_tables(smpp_server, smpp_database);
}

int smpp_database_mysql_remove(SMPPServer *smpp_server, unsigned long global_id, int temporary) {
    SMPPDatabase *smpp_database = smpp_server->database;
    Octstr *sql;
    DBPool *pool = smpp_database->context;
    Octstr *store_table;

    int res = 0;

    void *pending;
    Octstr *tmp = octstr_format("%lu", global_id);

    if(!temporary) {
        DBPoolConn *conn;

        store_table = dict_remove(smpp_database->pending_msg_store, tmp);
        if (store_table == NULL)
            store_table = octstr_duplicate(smpp_server->database_store_table);

        sql = octstr_format("DELETE FROM %S WHERE global_id = %lu", store_table, global_id);

        conn = dbpool_conn_consume(pool);

        if ((res = dbpool_conn_update(conn, sql, NULL)) == -1) {
            error(0, "Query error '%s'", octstr_get_cstr(sql));
            res = 0;
        } else {
            res = 1;
        }

        dbpool_conn_produce(conn);

        octstr_destroy(sql);
        octstr_destroy(store_table);
    }

    pending = dict_remove(smpp_database->pending_msg, tmp);
    if (!pending) {
        error(0, "No such pending store message %s! ", octstr_get_cstr(tmp));
    } else {
        res = 1;
    }

    dict_remove(smpp_database->pending_msg_store, tmp);

    octstr_destroy(tmp);
    return res;
}


int smpp_database_mysql_add_pdu(SMPPServer *smpp_server, SMPPQueuedPDU *smpp_queued_pdu) {
    SMPPDatabase *smpp_database = smpp_server->database;
    Octstr *sql;
    DBPool *pool = smpp_database->context;
    List *binds = gwlist_create();

    int res = 0;

    DBPoolConn *conn;
    Octstr *enc = NULL;

    {
        Octstr *packed = smpp_pdu_pack_without_command_length(smpp_queued_pdu->system_id, smpp_queued_pdu->pdu);
        enc = smpp_db_url_encode_octstr(packed);
        octstr_destroy(packed);
    }

    if (SMPP_DB_PGSQL(smpp_database)) {
        Octstr *sys_lit = smpp_db_sql_string_literal(smpp_queued_pdu->system_id);
        Octstr *pdu_lit = smpp_db_sql_string_literal(enc);
        sql = octstr_format("INSERT INTO %S (system_id, time, pdu) VALUES (%S, %ld, %S);",
                smpp_server->database_pdu_table, sys_lit, smpp_queued_pdu->time_sent, pdu_lit);
        octstr_destroy(sys_lit);
        octstr_destroy(pdu_lit);
    } else {
        sql = octstr_format("INSERT INTO %S ( `system_id`, `time`, `pdu` ) VALUES ( ?, ?, ?);", smpp_server->database_pdu_table);
        gwlist_produce(binds, octstr_duplicate(smpp_queued_pdu->system_id));
        gwlist_produce(binds, octstr_format("%ld", smpp_queued_pdu->time_sent));
        gwlist_produce(binds, enc);
        enc = NULL;
    }

    conn = dbpool_conn_consume(pool);

    if ((res = dbpool_conn_update(conn, sql, SMPP_DB_PGSQL(smpp_database) ? NULL : binds)) == -1) {
        error(0, "Query error '%s'", octstr_get_cstr(sql));
        res = 0;
    } else {
        res = 1;
    }

    dbpool_conn_produce(conn);

    gwlist_destroy(binds, octstr_destroy_item);
    octstr_destroy(enc);
    octstr_destroy(sql);

    return res;
}


int smpp_database_mysql_add_message(SMPPServer *smpp_server, Msg *msg, int store_kind) {
    SMPPDatabase *smpp_database = smpp_server->database;
    Octstr *sql;
    Octstr *values;
    Octstr *store_table;
    DBPool *pool = smpp_database->context;
    List *binds = gwlist_create();

    int res = 0;

    DBPoolConn *conn;

    char id[UUID_STR_LEN + 1];

    store_table = smpp_database_store_table_name(smpp_server, store_kind);

    if (msg->type == sms) {
        if (msg->sms.udhdata != NULL && octstr_len(msg->sms.udhdata) > 0) {
            Octstr *e = smpp_db_url_encode_octstr(msg->sms.udhdata);
            octstr_destroy(msg->sms.udhdata);
            msg->sms.udhdata = e;
        }
        if (msg->sms.msgdata != NULL && octstr_len(msg->sms.msgdata) > 0) {
            Octstr *e = smpp_db_url_encode_octstr(msg->sms.msgdata);
            octstr_destroy(msg->sms.msgdata);
            msg->sms.msgdata = e;
        }
    }

    sql = octstr_format("INSERT INTO %S ( ", store_table);

    values = octstr_create(" ) VALUES ( ");

#define INTEGER(name) octstr_append_cstr(sql, #name ","); smpp_db_sql_append_insert_long(values, binds, smpp_database, p->name);
#define OCTSTR(name) if(p->name != NULL) { octstr_append_cstr(sql, #name ","); smpp_db_sql_append_insert_value(values, binds, smpp_database, p->name); };
#define UUID(name) uuid_unparse(p->name, id); \
                octstr_append_cstr(sql, #name ","); \
                if (SMPP_DB_PGSQL(smpp_database)) { \
                    octstr_format_append(values, "'%s',", id); \
                } else { \
                    gwlist_produce(binds, octstr_format("%s", id)); \
                    octstr_append_cstr(values, "?,"); \
                }
#define VOID(name) ;
#define MSG(type, stmt) \
        case type: {struct type *p = &msg->type; stmt} break;
    switch (msg->type) {
#include "gw/msg-decl.h"
        default:
            return 0;
    }

    octstr_delete(sql, (octstr_len(sql) - 1), 1);
    octstr_delete(values, (octstr_len(values) - 1), 1);
    octstr_append(sql, values);
    octstr_append_cstr(sql, ");");

    conn = dbpool_conn_consume(pool);

    if ((res = dbpool_conn_update(conn, sql, SMPP_DB_PGSQL(smpp_database) ? NULL : binds)) == -1) {
        error(0, "Query error '%s'", octstr_get_cstr(sql));
        res = 0;
    } else {
        res = 1;
    }

    dbpool_conn_produce(conn);

    gwlist_destroy(binds, octstr_destroy_item);
    octstr_destroy(sql);
    octstr_destroy(values);
    octstr_destroy(store_table);

    return res;
}

int smpp_database_mysql_deduct_credit(SMPPServer *context, Octstr *service, double cost) {
    SMPPServer *smpp_server = context;
    SMPPDatabase *smpp_database = smpp_server->database;

    int balance_ok = 0;

    debug("smpp.database.mysql.deduct.credit", 0, "MySQL deducting credit from %s value %f", octstr_get_cstr(service), cost);

    DBPool *pool = smpp_database->context;
    Octstr *sql, *like = NULL;
    DBPoolConn *pconn;
    List *result = NULL, *row;

    pconn = dbpool_conn_consume(pool);
    if (pconn == NULL) /* should not happens, but sure is sure */
        return 0;

    List *binds = gwlist_create();

    sql = octstr_format("SELECT credit FROM %S WHERE ", smpp_server->database_user_table);
    smpp_db_sql_append_where_eq(sql, binds, smpp_database, "system_id", service);
    octstr_append_cstr(sql, " LIMIT 1");

    if (dbpool_conn_select(pconn, sql, SMPP_DB_PGSQL(smpp_database) ? NULL : binds, &result) != 0) {
        octstr_destroy(sql);
        gwlist_destroy(binds, (void(*)(void *))octstr_destroy);
        dbpool_conn_produce(pconn);
        return 0;
    }
    octstr_destroy(sql);
    octstr_destroy(like);
    gwlist_destroy(binds, NULL);

    double balance;
    Octstr *balance_str;
    Octstr *cost_str;


    if (gwlist_len(result) > 0) {
        row = gwlist_extract_first(result);
        balance_str = gwlist_get(row, 0);

        if(balance_str) {
            if(octstr_parse_double(&balance, balance_str, 0) != -1) {
                if(balance >= cost) {
                    balance_ok = 1;
                }
            }
        }
        gwlist_destroy(row, (void(*)(void *))octstr_destroy);
    }

    gwlist_destroy(result, NULL);

    if(balance_ok) {
        if(cost != 0.0) {
            cost_str = octstr_format("%f", cost);
            if (SMPP_DB_PGSQL(smpp_database)) {
                Octstr *svc_lit = smpp_db_sql_string_literal(service);
                sql = octstr_format("UPDATE %S SET credit = credit - %f WHERE system_id = %S",
                        smpp_server->database_user_table, cost, svc_lit);
                octstr_destroy(svc_lit);
            } else {
                sql = octstr_format("UPDATE %S SET `credit` = `credit` - ? WHERE `system_id` = ?", smpp_server->database_user_table);
                binds = gwlist_create();
                gwlist_append(binds, cost_str);
                gwlist_append(binds, service);
            }
            if(dbpool_conn_update(pconn, sql, SMPP_DB_PGSQL(smpp_database) ? NULL : binds) < 1) {
                error(0, "Error deducting %f credit from %s", cost, octstr_get_cstr(service));
                balance_ok = 0;
            }
            if (!SMPP_DB_PGSQL(smpp_database))
                gwlist_destroy(binds, NULL);
            octstr_destroy(sql);
            octstr_destroy(cost_str);
        } else {
            debug("smpp.database.mysql.deduct.credit", 0, "Cost is zero, no query to run");
        }
    }

    dbpool_conn_produce(pconn);


    return balance_ok;
}



SMPPESMEAuthResult *smpp_database_mysql_authenticate(void *context, Octstr *username, Octstr *password) {
    SMPPServer *smpp_server = context;
    SMPPDatabase *smpp_database = smpp_server->database;

    debug("smpp.database.mysql.authenticate", 0, "MySQL authenticating with %s:%s", octstr_get_cstr(username), octstr_get_cstr(password));

    DBPool *pool = smpp_database->context;
    Octstr *sql, *like = NULL;
    DBPoolConn *pconn;
    List *result = NULL, *row;
    SMPPESMEAuthResult *res = NULL;
    List *binds = gwlist_create();
    Octstr *tmp;

    pconn = dbpool_conn_consume(pool);
    if (pconn == NULL) /* should not happens, but sure is sure */
        return NULL;

    sql = octstr_format("SELECT "
            "throughput, "
            "default_smsc, "
            "callback_url, "
            "simulate, "
            "simulate_dlr_fail, "
            "simulate_deliver_every, "
            "simulate_permanent_failure_every, "
            "simulate_temporary_failure_every, "
            "simulate_mo_every, "
            "default_cost, "
            "max_binds, "
            "enable_prepaid_billing, "
            "connect_allow_ip "
            " FROM %S WHERE ", smpp_server->database_user_table);
    smpp_db_sql_append_where_eq(sql, binds, smpp_database, "system_id", username);
    smpp_db_sql_append_password_check(sql, binds, smpp_database, password);
    octstr_append_cstr(sql, " LIMIT 1");

    if (dbpool_conn_select(pconn, sql, SMPP_DB_PGSQL(smpp_database) ? NULL : binds, &result) != 0) {
        octstr_destroy(sql);
        gwlist_destroy(binds, NULL);
        dbpool_conn_produce(pconn);
        return NULL;
    }
    octstr_destroy(sql);
    octstr_destroy(like);
    gwlist_destroy(binds, NULL);
    dbpool_conn_produce(pconn);

    if (gwlist_len(result) > 0) {
        row = gwlist_extract_first(result);
        res = smpp_esme_auth_result_create();
        res->throughput = atof(octstr_get_cstr(gwlist_get(row, 0)));
        res->default_smsc = octstr_duplicate(gwlist_get(row, 1));
        res->callback_url = octstr_duplicate(gwlist_get(row, 2));
        res->default_cost = atof(octstr_get_cstr(gwlist_get(row, 9)));
        res->max_binds = atoi(octstr_get_cstr(gwlist_get(row, 10)));
        res->enable_prepaid_billing = atoi(octstr_get_cstr(gwlist_get(row, 11)));
        if(octstr_len(gwlist_get(row, 12))) {
            res->allowed_ips = octstr_duplicate(gwlist_get(row, 12));
        }

        tmp = gwlist_get(row, 3);
        if(tmp != NULL) {
            res->simulate = atoi(octstr_get_cstr(tmp));
            if(res->simulate) {
                tmp = gwlist_get(row, 4);
                if(tmp) {
                    res->simulate_dlr_fail = atoi(octstr_get_cstr(tmp));
                }
                tmp = gwlist_get(row, 5);
                if(tmp) {
                    res->simulate_deliver_every = atol(octstr_get_cstr(tmp));
                }
                tmp = gwlist_get(row, 6);
                if(tmp) {
                    res->simulate_permanent_failure_every = atol(octstr_get_cstr(tmp));
                }
                tmp = gwlist_get(row, 7);
                if(tmp) {
                    res->simulate_temporary_failure_every = atol(octstr_get_cstr(tmp));
                }
                tmp = gwlist_get(row, 8);
                if(tmp) {
                    res->simulate_mo_every = atol(octstr_get_cstr(tmp));
                }
            }
        }

        gwlist_destroy(row, octstr_destroy_item);
    }

    gwlist_destroy(result, NULL);


    return res;
}

void smpp_database_mysql_shutdown(SMPPServer *smpp_server) {
    SMPPDatabase *smpp_database = smpp_server->database;
    DBPool *pool = smpp_database->context;

    if (smpp_database->queue_context) {
        info(0, "Shutting down Redis queue connections");
        dbpool_destroy((DBPool *) smpp_database->queue_context);
        smpp_database->queue_context = NULL;
    }

    info(0, "Shutting down %s connections",
            SMPP_DB_PGSQL(smpp_database) ? "PostgreSQL" : "MySQL");
    dbpool_destroy(pool);

    dict_destroy(smpp_database->pending_msg);
    dict_destroy(smpp_database->pending_msg_store);
    dict_destroy(smpp_database->pending_pdu);
}

void *smpp_database_mysql_init(SMPPServer *smpp_server) {
    CfgGroup *grp = NULL;
    List *grplist;
    Octstr *mysql_host, *mysql_user, *mysql_pass, *mysql_db;
    long mysql_port = 0;
    Octstr *p = NULL;
    long pool_size;
    DBConf *db_conf = NULL;
    DBPool *pool;

    /*
     * now grap the required information from the 'mysql-connection' group
     * with the mysql-id we just obtained
     *
     * we have to loop through all available MySQL connection definitions
     * and search for the one we are looking for
     */

    grplist = cfg_get_multi_group(smpp_server->running_configuration, octstr_imm("mysql-connection"));
    while (grplist && (grp = gwlist_extract_first(grplist)) != NULL) {
        p = cfg_get(grp, octstr_imm("id"));
        if (p != NULL && octstr_compare(p, smpp_server->database_config) == 0) {
            goto found;
        }
        if (p != NULL) octstr_destroy(p);
    }
    panic(0, "DLR: MySQL: connection settings for id '%s' are not specified!",
            octstr_get_cstr(smpp_server->database_config));

found:
    octstr_destroy(p);
    gwlist_destroy(grplist, NULL);

    if (cfg_get_integer(&pool_size, grp, octstr_imm("max-connections")) == -1 || pool_size == 0)
        pool_size = 1;

    if (!(mysql_host = cfg_get(grp, octstr_imm("host"))))
        panic(0, "MySQL: directive 'host' is not specified!");
    if (!(mysql_user = cfg_get(grp, octstr_imm("username"))))
        panic(0, "MySQL: directive 'username' is not specified!");
    if (!(mysql_pass = cfg_get(grp, octstr_imm("password"))))
        panic(0, "MySQL: directive 'password' is not specified!");
    if (!(mysql_db = cfg_get(grp, octstr_imm("database"))))
        panic(0, "MySQL: directive 'database' is not specified!");

    cfg_get_integer(&mysql_port, grp, octstr_imm("port")); /* optional */

    /*
     * ok, ready to connect to MySQL
     */
    db_conf = gw_malloc(sizeof (DBConf));
    gw_assert(db_conf != NULL);

    db_conf->mysql = gw_malloc(sizeof (MySQLConf));
    gw_assert(db_conf->mysql != NULL);

    db_conf->mysql->host = mysql_host;
    db_conf->mysql->port = mysql_port;
    db_conf->mysql->username = mysql_user;
    db_conf->mysql->password = mysql_pass;
    db_conf->mysql->database = mysql_db;

    pool = dbpool_create(DBPOOL_MYSQL, db_conf, pool_size);
    gw_assert(pool != NULL);

    /*
     * XXX should a failing connect throw panic?!
     */
    if (dbpool_conn_count(pool) == 0)
        panic(0, "MySQL: database pool has no connections!");

    smpp_server->database = smpp_database_relational_setup(smpp_server, pool, DBPOOL_MYSQL);
    return smpp_server->database;
}

#ifdef HAVE_PGSQL
void *smpp_database_pgsql_init(SMPPServer *smpp_server) {
    CfgGroup *grp = NULL;
    List *grplist;
    Octstr *pgsql_host, *pgsql_user, *pgsql_pass, *pgsql_db;
    long pgsql_port = 0;
    Octstr *p = NULL;
    long pool_size;
    DBConf *db_conf = NULL;
    DBPool *pool;

    grplist = cfg_get_multi_group(smpp_server->running_configuration, octstr_imm("pgsql-connection"));
    while (grplist && (grp = gwlist_extract_first(grplist)) != NULL) {
        p = cfg_get(grp, octstr_imm("id"));
        if (p != NULL && octstr_compare(p, smpp_server->database_config) == 0) {
            goto pgsql_found;
        }
        if (p != NULL) octstr_destroy(p);
    }
    panic(0, "PostgreSQL: connection settings for id '%s' are not specified!",
            octstr_get_cstr(smpp_server->database_config));

pgsql_found:
    octstr_destroy(p);
    gwlist_destroy(grplist, NULL);

    if (cfg_get_integer(&pool_size, grp, octstr_imm("max-connections")) == -1 || pool_size == 0)
        pool_size = 1;

    if (!(pgsql_host = cfg_get(grp, octstr_imm("host"))))
        panic(0, "PostgreSQL: directive 'host' is not specified!");
    if (!(pgsql_user = cfg_get(grp, octstr_imm("username"))))
        panic(0, "PostgreSQL: directive 'username' is not specified!");
    if (!(pgsql_pass = cfg_get(grp, octstr_imm("password"))))
        panic(0, "PostgreSQL: directive 'password' is not specified!");
    if (!(pgsql_db = cfg_get(grp, octstr_imm("database"))))
        panic(0, "PostgreSQL: directive 'database' is not specified!");

    cfg_get_integer(&pgsql_port, grp, octstr_imm("port"));

    db_conf = gw_malloc(sizeof(DBConf));
    db_conf->pgsql = gw_malloc(sizeof(PgSQLConf));
    db_conf->pgsql->host = pgsql_host;
    db_conf->pgsql->port = pgsql_port;
    db_conf->pgsql->username = pgsql_user;
    db_conf->pgsql->password = pgsql_pass;
    db_conf->pgsql->database = pgsql_db;
    db_conf->pgsql->options = NULL;
    db_conf->pgsql->tty = NULL;

    pool = dbpool_create(DBPOOL_PGSQL, db_conf, pool_size);
    if (dbpool_conn_count(pool) == 0)
        panic(0, "PostgreSQL: database pool has no connections!");

    smpp_server->database = smpp_database_relational_setup(smpp_server, pool, DBPOOL_PGSQL);
    return smpp_server->database;
}
#endif

static SMPPDatabase *smpp_database_relational_setup(SMPPServer *smpp_server, DBPool *pool, enum db_type dialect)
{
    SMPPDatabase *smpp_database = smpp_database_create();
    smpp_database->sql_dialect = dialect;
    smpp_database->authenticate = smpp_database_mysql_authenticate;
    smpp_database->add_message = smpp_database_mysql_add_message;
    smpp_database->get_stored = smpp_database_mysql_get_stored;
    smpp_database->get_dlrs = smpp_database_mysql_get_dlrs;
    smpp_database->delete = smpp_database_mysql_remove;
    smpp_database->delete_dlr = smpp_database_mysql_remove_dlr;
    smpp_database->add_pdu = smpp_database_mysql_add_pdu;
    smpp_database->get_stored_pdu = smpp_database_mysql_get_stored_pdu;
    smpp_database->get_routes = smpp_database_mysql_get_routes;
    smpp_database->shutdown = smpp_database_mysql_shutdown;
    smpp_database->deduct_credit = smpp_database_mysql_deduct_credit;
    smpp_database->get_esmes_with_queued = smpp_database_mysql_get_esmes_with_queued;
    smpp_database->context = pool;
    smpp_database->pending_pdu = dict_create(1024, NULL);
    smpp_database->pending_msg = dict_create(1024, NULL);
    smpp_database->pending_msg_store = dict_create(1024, (void(*)(void *))octstr_destroy);

    if(!octstr_len(smpp_server->database_user_table)) {
        warning(0, "No 'database-user-table' specified, using default 'smpp_user'");
        octstr_destroy(smpp_server->database_user_table);
        smpp_server->database_user_table = octstr_create("smpp_user");
    }

    if(!octstr_len(smpp_server->database_store_table)) {
        warning(0, "No 'database-store-table' specified, using default 'smpp_store'");
        octstr_destroy(smpp_server->database_store_table);
        smpp_server->database_store_table = octstr_create("smpp_store");
    }

    if(!octstr_len(smpp_server->database_queue_store_table)) {
        octstr_destroy(smpp_server->database_queue_store_table);
        smpp_server->database_queue_store_table = octstr_create("smpp_store_queue");
        info(0, "Using default 'smpp_store_queue' table for bearerbox/SMSC fallback queue");
    }

    if(smpp_server->database_store_primary) {
        info(0, "Store-primary MT uses '%s'; bearerbox fallback queue uses '%s'",
                octstr_get_cstr(smpp_server->database_store_table),
                octstr_get_cstr(smpp_server->database_queue_store_table));
    }

    if(!smpp_server->database_dlr_table || !octstr_len(smpp_server->database_dlr_table)) {
        octstr_destroy(smpp_server->database_dlr_table);
        smpp_server->database_dlr_table = octstr_create("smpp_dlr");
        info(0, "Using default 'smpp_dlr' table for DLR storage (set database-dlr-table to empty to disable)");
    }

     if(!octstr_len(smpp_server->database_pdu_table)) {
        warning(0, "No 'database-pdu-table' specified, using default 'smpp_queued_pdu'");
        octstr_destroy(smpp_server->database_pdu_table);
        smpp_server->database_pdu_table = octstr_create("smpp_queued_pdu");
    }

    if(!octstr_len(smpp_server->database_route_table)) {
        warning(0, "No 'database-route-table' specified, using default 'smpp_route'");
        octstr_destroy(smpp_server->database_route_table);
        smpp_server->database_route_table = octstr_create("smpp_route");
    }

    if(!octstr_len(smpp_server->database_version_table)) {
        warning(0, "No 'database-version-table' specified, using default 'smpp_version'");
        octstr_destroy(smpp_server->database_version_table);
        smpp_server->database_version_table = octstr_create("smpp_version");
    }

    smpp_database_sql_init_tables(smpp_server, smpp_database);

    if (octstr_len(smpp_server->database_queue_type)
            && octstr_case_compare(smpp_server->database_queue_type, octstr_imm("redis")) == 0) {
        smpp_database_redis_queue_attach(smpp_server, smpp_database);
    }

    return smpp_database;
}
