#include "gwlib/gwlib.h"
#include "gwlib/dbpool.h"
#include <stdarg.h>
#include "gw/msg.h"
#include "gw/load.h"
#include "gw/dlr.h"
#include "gw/smsc/smpp_pdu.h"
#include "smpp_server.h"
#include "smpp_bearerbox.h"
#include "smpp_esme.h"
#include "smpp_queued_pdu.h"
#include "smpp_database.h"
#include "smpp_database_util.h"
#include "smpp_database_redis.h"

#ifdef HAVE_REDIS
#include <ctype.h>
#include "hiredis.h"
#include "jansson.h"

typedef struct {
    Octstr *json;
    Octstr *list_key;
    Octstr *store_table;
    Octstr *service;
    long sms_type;
} SMPPRedisPendingEntry;

static void smpp_redis_pending_entry_destroy(SMPPRedisPendingEntry *entry)
{
    if (entry == NULL)
        return;
    octstr_destroy(entry->json);
    octstr_destroy(entry->list_key);
    octstr_destroy(entry->store_table);
    octstr_destroy(entry->service);
    gw_free(entry);
}

static void *smpp_redis_json_malloc(size_t size)
{
    return gw_malloc(size);
}

static void smpp_redis_json_free(void *ptr)
{
    gw_free(ptr);
}

static DBPool *smpp_redis_pool(SMPPDatabase *smpp_database)
{
    return (DBPool *) smpp_database->queue_context;
}

static redisReply *smpp_redis_command(DBPool *pool, const char *fmt, ...)
{
    redisReply *reply = NULL;
    DBPoolConn *pc;
    va_list ap;
    Octstr *cmd;

    va_start(ap, fmt);
    cmd = octstr_format_valist_real(fmt, ap);
    va_end(ap);

    pc = dbpool_conn_consume(pool);
    if (pc == NULL) {
        error(0, "REDIS: database pool has no connection");
        octstr_destroy(cmd);
        return NULL;
    }

    reply = redisCommand(pc->conn, octstr_get_cstr(cmd));
    if (reply != NULL && reply->type == REDIS_REPLY_ERROR) {
        error(0, "REDIS: %s", reply->str);
    }

    octstr_destroy(cmd);
    dbpool_conn_produce(pc);
    return reply;
}

static int smpp_redis_check_version(DBPool *pool)
{
    redisReply *reply;
    int major = 0, minor = 0, patch = 0;
    const char *ver;

    reply = smpp_redis_command(pool, "INFO server");
    if (reply == NULL || reply->type != REDIS_REPLY_STRING) {
        if (reply)
            freeReplyObject(reply);
        panic(0, "REDIS: unable to read server INFO for version check");
    }

    ver = strstr(reply->str, "redis_version:");
    if (ver != NULL) {
        sscanf(ver, "redis_version:%d.%d.%d", &major, &minor, &patch);
    }
    freeReplyObject(reply);

    if (major < 6 || (major == 6 && minor < 2)) {
        panic(0, "REDIS: Redis %d.%d.%d detected; LPOP count requires Redis 6.2+", major, minor, patch);
    }
    info(0, "REDIS: connected to Redis %d.%d.%d", major, minor, patch);
    return 1;
}

static Octstr *smpp_redis_norm_service(Octstr *service)
{
    Octstr *out;

    if (service == NULL || !octstr_len(service))
        return octstr_create(SMPP_REDIS_GLOBAL_SERVICE);
    out = octstr_duplicate(service);
    octstr_convert_range(out, 0, octstr_len(out), tolower);
    return out;
}

static Octstr *smpp_redis_store_queue_key_for(Octstr *store_table)
{
    return octstr_format("%S:queue", store_table);
}

static Octstr *smpp_redis_store_esmes_key_for(Octstr *store_table)
{
    return octstr_format("%S:esmes", store_table);
}

static Octstr *smpp_redis_store_id_key_for(Octstr *store_table)
{
    return octstr_format("%S:id", store_table);
}

static Octstr *smpp_redis_pdu_list_key(SMPPServer *smpp_server, Octstr *system_id)
{
    Octstr *sid = smpp_redis_norm_service(system_id);
    Octstr *key = octstr_format("%S:%S", smpp_server->database_pdu_table, sid);
    octstr_destroy(sid);
    return key;
}

static Octstr *smpp_redis_pdu_esmes_key(SMPPServer *smpp_server)
{
    return octstr_format("%S:esmes", smpp_server->database_pdu_table);
}

static Octstr *smpp_redis_pdu_id_key(SMPPServer *smpp_server)
{
    return octstr_format("%S:id", smpp_server->database_pdu_table);
}

static Octstr *smpp_redis_dlr_list_key(SMPPServer *smpp_server, Octstr *service)
{
    Octstr *svc = smpp_redis_norm_service(service);
    Octstr *key = octstr_format("%S:%S", smpp_server->database_dlr_table, svc);
    octstr_destroy(svc);
    return key;
}

static Octstr *smpp_redis_dlr_esmes_key(SMPPServer *smpp_server)
{
    return octstr_format("%S:esmes", smpp_server->database_dlr_table);
}

static Octstr *smpp_redis_dlr_id_key(SMPPServer *smpp_server)
{
    return octstr_format("%S:id", smpp_server->database_dlr_table);
}

static unsigned long smpp_redis_incr_id(DBPool *pool, Octstr *counter_key)
{
    redisReply *reply;
    unsigned long id = 0;

    reply = smpp_redis_command(pool, "INCR %s", octstr_get_cstr(counter_key));
    if (reply != NULL && reply->type == REDIS_REPLY_INTEGER)
        id = (unsigned long) reply->integer;
    if (reply)
        freeReplyObject(reply);
    return id;
}

static void smpp_redis_json_set_str(json_t *obj, const char *key, Octstr *val)
{
    if (val == NULL || octstr_len(val) == 0)
        json_object_set_new(obj, key, json_string(""));
    else
        json_object_set_new(obj, key, json_string(octstr_get_cstr(val)));
}

static void smpp_redis_json_set_long(json_t *obj, const char *key, long val)
{
    if (val == -1)
        json_object_set_new(obj, key, json_null());
    else
        json_object_set_new(obj, key, json_integer(val));
}

static long smpp_redis_json_get_long(json_t *obj, const char *key)
{
    json_t *val = json_object_get(obj, key);
    if (val == NULL || json_is_null(val))
        return -1;
    if (json_is_integer(val))
        return (long) json_integer_value(val);
    if (json_is_real(val))
        return (long) json_real_value(val);
    if (json_is_string(val))
        return atol(json_string_value(val));
    return -1;
}

static Octstr *smpp_redis_json_get_octstr(json_t *obj, const char *key)
{
    json_t *val = json_object_get(obj, key);
    if (val == NULL || json_is_null(val))
        return octstr_create("");
    if (json_is_string(val))
        return octstr_create(json_string_value(val));
    return octstr_create("");
}

static Octstr *smpp_redis_msg_to_json(Msg *msg, unsigned long global_id)
{
    json_t *root = json_object();
    char id[UUID_STR_LEN + 1];
    char *json;
    Octstr *jsonstr;

    json_object_set_new(root, "global_id", json_integer((json_int_t) global_id));

#define INTEGER(name) json_object_set_new(root, #name, json_integer((json_int_t) p->name));
#define OCTSTR(name) smpp_redis_json_set_str(root, #name, p->name);
#define UUID(name) uuid_unparse(p->name, id); json_object_set_new(root, #name, json_string(id));
#define VOID(name) ;
#define MSG(type, stmt) \
        case type: { struct type *p = &msg->type; stmt; } break;
    switch (msg->type) {
#include "gw/msg-decl.h"
        default:
            json_decref(root);
            return NULL;
    }

    json = json_dumps(root, JSON_COMPACT);
    jsonstr = octstr_create(json);
    json_decref(root);
    gw_free(json);
    return jsonstr;
}

static Msg *smpp_redis_json_to_msg(json_t *root, Msg *msg)
{
    char id[UUID_STR_LEN + 1];
    json_t *val;

    if (msg == NULL)
        msg = msg_create(sms);

#define INTEGER(name) val = json_object_get(root, #name); if (val && !json_is_null(val)) p->name = smpp_redis_json_get_long(root, #name);
#define OCTSTR(name) octstr_destroy(p->name); p->name = smpp_redis_json_get_octstr(root, #name);
#define UUID(name) val = json_object_get(root, #name); if (val && json_is_string(val)) uuid_parse(json_string_value(val), p->name);
#define VOID(name) ;
#define MSG(type, stmt) \
        case type: { struct type *p = &msg->type; stmt; } break;
    switch (msg->type) {
#include "gw/msg-decl.h"
        default:
            return msg;
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
    if (msg->sms.msgdata == NULL)
        msg->sms.msgdata = octstr_create("");

    return msg;
}

static int smpp_redis_lpush(DBPool *pool, Octstr *list_key, Octstr *payload)
{
    redisReply *reply;
    int ok = 0;

    reply = smpp_redis_command(pool, SMPP_REDIS_QUEUE_PUSH,
            octstr_get_cstr(list_key), octstr_get_cstr(payload));
    if (reply != NULL && reply->type != REDIS_REPLY_ERROR)
        ok = 1;
    if (reply)
        freeReplyObject(reply);
    return ok;
}

static List *smpp_redis_lpop_json_batch(DBPool *pool, Octstr *list_key, long limit)
{
    redisReply *reply;
    List *items = gwlist_create();
    long i;

    if (limit <= 0)
        limit = SMPP_DATABASE_BATCH_LIMIT;

    reply = smpp_redis_command(pool, SMPP_REDIS_QUEUE_POP,
            octstr_get_cstr(list_key), limit);
    if (reply == NULL)
        return items;

    if (reply->type == REDIS_REPLY_STRING) {
        gwlist_produce(items, octstr_create(reply->str));
    } else if (reply->type == REDIS_REPLY_ARRAY) {
        for (i = 0; i < (long) reply->elements; i++) {
            if (reply->element[i]->type == REDIS_REPLY_STRING)
                gwlist_produce(items, octstr_create(reply->element[i]->str));
        }
    }

    freeReplyObject(reply);
    return items;
}

static void smpp_redis_track_store_esme(DBPool *pool, Octstr *store_table, Octstr *service)
{
    Octstr *svc = smpp_redis_norm_service(service);
    Octstr *esmes_key = smpp_redis_store_esmes_key_for(store_table);
    redisReply *reply;

    reply = smpp_redis_command(pool, "SADD %s %s", octstr_get_cstr(esmes_key), octstr_get_cstr(svc));
    if (reply)
        freeReplyObject(reply);

    octstr_destroy(svc);
    octstr_destroy(esmes_key);
}

static int smpp_redis_store_matches(Msg *msg, long sms_type, Octstr *service_filter)
{
    Octstr *norm_msg_svc, *norm_want;
    int ok;

    if (msg == NULL || msg->type != sms)
        return 0;
    if (msg->sms.sms_type != sms_type)
        return 0;
    if (!octstr_len(service_filter))
        return 1;

    norm_want = smpp_redis_norm_service(service_filter);
    norm_msg_svc = smpp_redis_norm_service(msg->sms.service);
    ok = (octstr_compare(norm_want, norm_msg_svc) == 0);
    octstr_destroy(norm_want);
    octstr_destroy(norm_msg_svc);
    return ok;
}

static int smpp_database_redis_add_message(SMPPServer *smpp_server, Msg *msg, int store_kind)
{
    SMPPDatabase *smpp_database = smpp_server->database;
    DBPool *pool = smpp_redis_pool(smpp_database);
    Octstr *list_key, *jsonstr, *id_key, *store_table;
    unsigned long global_id;
    int res = 0;

    if (msg->type != sms)
        return 0;

    store_table = smpp_database_store_table_name(smpp_server, store_kind);

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

    id_key = smpp_redis_store_id_key_for(store_table);
    global_id = smpp_redis_incr_id(pool, id_key);
    octstr_destroy(id_key);

    jsonstr = smpp_redis_msg_to_json(msg, global_id);
    if (jsonstr == NULL) {
        octstr_destroy(store_table);
        return 0;
    }

    list_key = smpp_redis_store_queue_key_for(store_table);
    res = smpp_redis_lpush(pool, list_key, jsonstr);
    if (res)
        smpp_redis_track_store_esme(pool, store_table, msg->sms.service);

    octstr_destroy(list_key);
    octstr_destroy(jsonstr);
    octstr_destroy(store_table);
    return res;
}

static int smpp_redis_claim_store_message(SMPPServer *smpp_server, Octstr *list_key,
        Octstr *store_table, Octstr *jsonstr, long sms_type, Octstr *service_filter, List *messages)
{
    SMPPDatabase *smpp_database = smpp_server->database;
    SMPPDatabaseMsg *smpp_database_msg;
    SMPPRedisPendingEntry *pending_entry;
    json_t *root;
    json_error_t json_error;
    Msg *msg;
    Octstr *id_key;

    root = json_loads(octstr_get_cstr(jsonstr), 0, &json_error);
    if (!json_is_object(root)) {
        error(0, "REDIS: invalid store JSON: %s", octstr_get_cstr(jsonstr));
        if (root)
            json_decref(root);
        return 0;
    }

    msg = smpp_redis_json_to_msg(root, msg_create(sms));
    if (!smpp_redis_store_matches(msg, sms_type, service_filter)) {
        msg_destroy(msg);
        json_decref(root);
        return 0;
    }

    smpp_database_msg = smpp_database_msg_create();
    smpp_database_msg->global_id = (unsigned long) smpp_redis_json_get_long(root, "global_id");
    smpp_database_msg->smpp_server = smpp_server;
    smpp_database_msg->msg = msg;

    pending_entry = gw_malloc(sizeof(SMPPRedisPendingEntry));
    pending_entry->json = octstr_duplicate(jsonstr);
    pending_entry->list_key = octstr_duplicate(list_key);
    pending_entry->store_table = octstr_duplicate(store_table);
    pending_entry->service = octstr_duplicate(msg->sms.service);
    pending_entry->sms_type = msg->sms.sms_type;
    smpp_database_msg->store_table = octstr_duplicate(store_table);

    id_key = octstr_format("%lu", smpp_database_msg->global_id);
    dict_put(smpp_database->pending_msg, id_key, pending_entry);
    dict_put(smpp_database->pending_msg_store, octstr_duplicate(id_key), octstr_duplicate(store_table));
    gwlist_produce(messages, smpp_database_msg);

    json_decref(root);
    return 1;
}

List *smpp_database_redis_get_stored(SMPPServer *smpp_server, long sms_type, Octstr *service, long limit, int store_kind)
{
    DBPool *pool = smpp_redis_pool(smpp_server->database);
    List *messages = gwlist_create();
    List *raw_items;
    Octstr *list_key, *jsonstr, *store_table;
    long picked = 0;
    long empty_batches = 0;
    long max_empty_batches = 32;

    if (!limit)
        limit = SMPP_DATABASE_BATCH_LIMIT;

    store_table = (store_kind >= 0)
            ? smpp_database_store_table_name(smpp_server, store_kind)
            : smpp_database_get_stored_table_name(smpp_server, sms_type);
    list_key = smpp_redis_store_queue_key_for(store_table);

    while (picked < limit && empty_batches < max_empty_batches) {
        long batch_picked = 0;

        raw_items = smpp_redis_lpop_json_batch(pool, list_key, limit - picked);
        if (gwlist_len(raw_items) == 0) {
            gwlist_destroy(raw_items, NULL);
            break;
        }

        while ((jsonstr = gwlist_extract_first(raw_items)) != NULL) {
            if (smpp_redis_claim_store_message(smpp_server, list_key, store_table, jsonstr, sms_type, service, messages)) {
                picked++;
                batch_picked++;
            } else {
                smpp_redis_lpush(pool, list_key, jsonstr);
            }
            octstr_destroy(jsonstr);
        }
        gwlist_destroy(raw_items, NULL);

        if (batch_picked == 0)
            empty_batches++;
        else
            empty_batches = 0;
    }

    octstr_destroy(list_key);
    octstr_destroy(store_table);
    return messages;
}

int smpp_database_redis_remove(SMPPServer *smpp_server, unsigned long global_id, int temporary)
{
    SMPPDatabase *smpp_database = smpp_server->database;
    Octstr *tmp = octstr_format("%lu", global_id);
    SMPPRedisPendingEntry *pending_entry;
    int res = 0;

    pending_entry = dict_remove(smpp_database->pending_msg, tmp);
    if (pending_entry == NULL) {
        error(0, "REDIS: no pending store message %lu", global_id);
        octstr_destroy(tmp);
        return 0;
    }

    if (temporary && pending_entry->json != NULL && pending_entry->list_key != NULL) {
        DBPool *pool = smpp_redis_pool(smpp_database);
        if (smpp_redis_lpush(pool, pending_entry->list_key, pending_entry->json)) {
            res = 1;
            smpp_redis_track_store_esme(pool, pending_entry->store_table, pending_entry->service);
        }
    } else {
        res = 1;
    }

    smpp_redis_pending_entry_destroy(pending_entry);
    dict_remove(smpp_database->pending_msg_store, tmp);
    octstr_destroy(tmp);
    return res;
}

int smpp_database_redis_add_pdu(SMPPServer *smpp_server, SMPPQueuedPDU *smpp_queued_pdu)
{
    SMPPDatabase *smpp_database = smpp_server->database;
    DBPool *pool = smpp_redis_pool(smpp_database);
    Octstr *list_key, *jsonstr, *id_key, *packed, *enc;
    unsigned long global_id;
    json_t *root;
    char *json;
    int res = 0;

    id_key = smpp_redis_pdu_id_key(smpp_server);
    global_id = smpp_redis_incr_id(pool, id_key);
    octstr_destroy(id_key);

    packed = smpp_pdu_pack_without_command_length(smpp_queued_pdu->system_id, smpp_queued_pdu->pdu);
    enc = smpp_db_url_encode_octstr(packed);
    octstr_destroy(packed);

    root = json_object();
    json_object_set_new(root, "global_id", json_integer((json_int_t) global_id));
    json_object_set_new(root, "time", json_integer((json_int_t) smpp_queued_pdu->time_sent));
    smpp_redis_json_set_str(root, "system_id", smpp_queued_pdu->system_id);
    smpp_redis_json_set_str(root, "pdu", enc);
    json = json_dumps(root, JSON_COMPACT);
    jsonstr = octstr_create(json);
    json_decref(root);
    octstr_destroy(enc);
    gw_free(json);

    list_key = smpp_redis_pdu_list_key(smpp_server, smpp_queued_pdu->system_id);
    res = smpp_redis_lpush(pool, list_key, jsonstr);
    if (res) {
        Octstr *esmes_key = smpp_redis_pdu_esmes_key(smpp_server);
        Octstr *sid = smpp_redis_norm_service(smpp_queued_pdu->system_id);
        redisReply *reply = smpp_redis_command(pool, "SADD %s %s", octstr_get_cstr(esmes_key), octstr_get_cstr(sid));
        if (reply)
            freeReplyObject(reply);
        octstr_destroy(esmes_key);
        octstr_destroy(sid);
    }

    octstr_destroy(list_key);
    octstr_destroy(jsonstr);
    return res;
}

static void smpp_database_redis_queued_pdu_handler(void *context, long status)
{
    SMPPQueuedPDU *smpp_queued_pdu = context;

    if (!smpp_queued_pdu->smpp_server) {
        error(0, "No SMPP Server context, can't proceed");
        return;
    }

    SMPPDatabase *smpp_database = smpp_queued_pdu->smpp_server->database;

    if (status != SMPP_QUEUED_PDU_DESTROYED && status != SMPP_ESME_COMMAND_STATUS_WAIT_ACK_TIMEOUT) {
        /* PDU already removed from Redis on LPOP; nothing to delete in store */
    }

    dict_remove(smpp_database->pending_pdu, smpp_queued_pdu->bearerbox_id);
    smpp_queued_pdu_destroy(smpp_queued_pdu);
}

List *smpp_database_redis_get_stored_pdu(SMPPServer *smpp_server, Octstr *service, long limit)
{
    SMPPDatabase *smpp_database = smpp_server->database;
    DBPool *pool = smpp_redis_pool(smpp_database);
    Octstr *list_key;
    List *raw_items, *messages = gwlist_create();
    Octstr *jsonstr;
    json_t *root;
    json_error_t json_error;
    SMPPQueuedPDU *smpp_queued_pdu;
    Octstr *global_id_str;

    if (!limit)
        limit = SMPP_DATABASE_BATCH_LIMIT;

    list_key = smpp_redis_pdu_list_key(smpp_server, service);
    raw_items = smpp_redis_lpop_json_batch(pool, list_key, limit);

    while ((jsonstr = gwlist_extract_first(raw_items)) != NULL) {
        root = json_loads(octstr_get_cstr(jsonstr), 0, &json_error);
        if (!json_is_object(root)) {
            octstr_destroy(jsonstr);
            if (root)
                json_decref(root);
            continue;
        }

        smpp_queued_pdu = smpp_queued_pdu_create();
        global_id_str = octstr_format("%ld", smpp_redis_json_get_long(root, "global_id"));
        smpp_queued_pdu->bearerbox_id = global_id_str;
        smpp_queued_pdu->callback = smpp_database_redis_queued_pdu_handler;
        smpp_queued_pdu->context = smpp_queued_pdu;
        smpp_queued_pdu->system_id = octstr_duplicate(service);
        smpp_queued_pdu->time_sent = smpp_redis_json_get_long(root, "time");
        {
            Octstr *pdu_os = smpp_db_url_decode_octstr(smpp_redis_json_get_octstr(root, "pdu"));
            smpp_queued_pdu->pdu = smpp_pdu_unpack(service, pdu_os);
            octstr_destroy(pdu_os);
        }
        smpp_queued_pdu->smpp_server = smpp_server;

        dict_put(smpp_database->pending_pdu, octstr_duplicate(global_id_str), smpp_queued_pdu);
        gwlist_produce(messages, smpp_queued_pdu);

        json_decref(root);
        octstr_destroy(jsonstr);
    }
    gwlist_destroy(raw_items, NULL);

    if (gwlist_len(messages) == 0) {
        Octstr *esmes_key = smpp_redis_pdu_esmes_key(smpp_server);
        Octstr *sid = smpp_redis_norm_service(service);
        redisReply *reply = smpp_redis_command(pool, "LLEN %s", octstr_get_cstr(list_key));
        if (reply != NULL && reply->type == REDIS_REPLY_INTEGER && reply->integer == 0) {
            redisReply *r2 = smpp_redis_command(pool, "SREM %s %s", octstr_get_cstr(esmes_key), octstr_get_cstr(sid));
            if (r2)
                freeReplyObject(r2);
        }
        if (reply)
            freeReplyObject(reply);
        octstr_destroy(esmes_key);
        octstr_destroy(sid);
    }

    octstr_destroy(list_key);
    return messages;
}

List *smpp_database_redis_get_dlrs(SMPPServer *smpp_server, Octstr *service, long limit)
{
    SMPPDatabase *smpp_database = smpp_server->database;
    DBPool *pool = smpp_redis_pool(smpp_database);
    Octstr *list_key;
    List *raw_items, *messages = gwlist_create();
    Octstr *jsonstr;
    json_t *root;
    json_error_t json_error;
    SMPPDatabaseMsg *smpp_database_msg;
    Msg *msg;
    long dlr_mask;
    Octstr *msgdata, *dlr_url, *submit_date_os, *done_date_os, *text_os, *stat_os, *err_os;
    time_t submit_time, done_time;

    if (!octstr_len(smpp_server->database_dlr_table))
        return messages;

    if (!limit)
        limit = SMPP_DATABASE_BATCH_LIMIT;

    list_key = smpp_redis_dlr_list_key(smpp_server, service);
    raw_items = smpp_redis_lpop_json_batch(pool, list_key, limit);

    while ((jsonstr = gwlist_extract_first(raw_items)) != NULL) {
        root = json_loads(octstr_get_cstr(jsonstr), 0, &json_error);
        if (!json_is_object(root)) {
            octstr_destroy(jsonstr);
            if (root)
                json_decref(root);
            continue;
        }

        smpp_database_msg = smpp_database_msg_create();
        smpp_database_msg->global_id = (unsigned long) smpp_redis_json_get_long(root, "global_id");
        smpp_database_msg->smpp_server = smpp_server;

        msg = msg_create(sms);
        msg->sms.sms_type = report_mo;
        msg->sms.service = octstr_duplicate(service);
        msg->sms.receiver = smpp_redis_json_get_octstr(root, "destination_addr");
        msg->sms.sender = smpp_redis_json_get_octstr(root, "source_addr");
        msg->sms.smsc_id = smpp_redis_json_get_octstr(root, "smsc_id");

        submit_date_os = smpp_database_format_dlr_date(smpp_redis_json_get_octstr(root, "submit_date"));
        done_date_os = smpp_database_format_dlr_date(smpp_redis_json_get_octstr(root, "done_date"));
        text_os = smpp_redis_json_get_octstr(root, "text");

        submit_time = smpp_time_to_c_time(octstr_get_cstr(submit_date_os));
        done_time = smpp_time_to_c_time(octstr_get_cstr(done_date_os));
        if (submit_time > 0)
            msg->sms.time = submit_time;
        else if (done_time > 0)
            msg->sms.time = done_time;
        else
            msg->sms.time = time(NULL);

        stat_os = smpp_redis_json_get_octstr(root, "status");
        if (octstr_case_compare(stat_os, octstr_imm("DELIVRD")) == 0)
            dlr_mask = DLR_SUCCESS;
        else
            dlr_mask = DLR_FAIL;
        msg->sms.dlr_mask = dlr_mask;

        err_os = smpp_redis_json_get_octstr(root, "err_code");
        {
            Octstr *dlvrd_str = (dlr_mask == DLR_SUCCESS) ? octstr_imm("001") : octstr_imm("000");
            unsigned int err_val = 0;
            if (octstr_len(err_os)) {
                long tmp;
                octstr_parse_long(&tmp, err_os, 0, 10);
                err_val = (unsigned int) tmp;
            }
            msgdata = octstr_format("id:%S sub:001 dlvrd:%S submit date:%S done date:%S stat:%S err:%03x text:%S",
                    smpp_redis_json_get_octstr(root, "message_id"),
                    dlvrd_str,
                    submit_date_os,
                    done_date_os,
                    octstr_len(stat_os) ? stat_os : octstr_imm("DELIVRD"),
                    err_val,
                    text_os);
        }
        msg->sms.msgdata = msgdata;

        dlr_url = octstr_format("%S|%ld|%S", service, (long) msg->sms.time,
                smpp_redis_json_get_octstr(root, "message_id"));
        msg->sms.dlr_url = dlr_url;

        smpp_database_msg->msg = msg;
        gwlist_produce(messages, smpp_database_msg);

        octstr_destroy(submit_date_os);
        octstr_destroy(done_date_os);
        octstr_destroy(stat_os);
        octstr_destroy(err_os);
        octstr_destroy(text_os);
        json_decref(root);
        octstr_destroy(jsonstr);
    }

    gwlist_destroy(raw_items, NULL);
    octstr_destroy(list_key);
    return messages;
}

int smpp_database_redis_remove_dlr(SMPPServer *smpp_server, unsigned long global_id)
{
    /* DLR entries are removed from Redis on LPOP during get_dlrs */
    (void) smpp_server;
    (void) global_id;
    return 1;
}

static void smpp_redis_merge_string_esmes(redisReply *reply, List *esmes)
{
    long i;

    if (reply == NULL || reply->type != REDIS_REPLY_ARRAY)
        return;
    for (i = 0; i < (long) reply->elements; i++) {
        if (reply->element[i]->type == REDIS_REPLY_STRING)
            gwlist_produce(esmes, octstr_create(reply->element[i]->str));
    }
}

static void smpp_redis_merge_store_esmes(redisReply *reply, List *esmes)
{
    long i;

    if (reply == NULL || reply->type != REDIS_REPLY_ARRAY)
        return;
    for (i = 0; i < (long) reply->elements; i++) {
        Octstr *id;
        if (reply->element[i]->type != REDIS_REPLY_STRING)
            continue;
        id = octstr_create(reply->element[i]->str);
        if (octstr_case_compare(id, octstr_imm(SMPP_REDIS_GLOBAL_SERVICE)) != 0)
            gwlist_produce(esmes, id);
        else
            octstr_destroy(id);
    }
}

static void smpp_redis_add_store_esmes(DBPool *pool, Octstr *store_table, List *esmes)
{
    Octstr *key = smpp_redis_store_esmes_key_for(store_table);
    redisReply *reply = smpp_redis_command(pool, "SMEMBERS %s", octstr_get_cstr(key));

    smpp_redis_merge_store_esmes(reply, esmes);
    if (reply)
        freeReplyObject(reply);
    octstr_destroy(key);
}

List *smpp_database_redis_get_esmes_with_queued(SMPPServer *smpp_server)
{
    DBPool *pool = smpp_redis_pool(smpp_server->database);
    List *esmes = gwlist_create();
    Octstr *key;
    redisReply *reply;

    key = smpp_redis_pdu_esmes_key(smpp_server);
    reply = smpp_redis_command(pool, "SMEMBERS %s", octstr_get_cstr(key));
    smpp_redis_merge_string_esmes(reply, esmes);
    if (reply)
        freeReplyObject(reply);
    octstr_destroy(key);

    smpp_redis_add_store_esmes(pool, smpp_server->database_store_table, esmes);
    if (octstr_compare(smpp_server->database_store_table, smpp_server->database_queue_store_table) != 0)
        smpp_redis_add_store_esmes(pool, smpp_server->database_queue_store_table, esmes);

    if (octstr_len(smpp_server->database_dlr_table)) {
        key = smpp_redis_dlr_esmes_key(smpp_server);
        reply = smpp_redis_command(pool, "SMEMBERS %s", octstr_get_cstr(key));
        smpp_redis_merge_string_esmes(reply, esmes);
        if (reply)
            freeReplyObject(reply);
        octstr_destroy(key);
    }

    return esmes;
}

int smpp_database_redis_queue_attach(SMPPServer *smpp_server, SMPPDatabase *smpp_database)
{
    CfgGroup *grp = NULL;
    List *grplist;
    Octstr *redis_host, *redis_password, *p = NULL;
    long pool_size, redis_port = 0, redis_database = -1, redis_idle_timeout = -1;
    DBConf *db_conf = NULL;
    DBPool *pool;

    if (!octstr_len(smpp_server->database_queue_config))
        panic(0, "REDIS: database-queue-config is not specified");

    grplist = cfg_get_multi_group(smpp_server->running_configuration, octstr_imm("redis-connection"));
    if (!grplist)
        panic(0, "REDIS: no redis-connection groups in configuration");

    while (grplist && (grp = gwlist_extract_first(grplist)) != NULL) {
        p = cfg_get(grp, octstr_imm("id"));
        if (p != NULL && octstr_compare(p, smpp_server->database_queue_config) == 0)
            goto found;
        if (p != NULL)
            octstr_destroy(p);
    }
    panic(0, "REDIS: connection settings for id '%s' are not specified!",
            octstr_get_cstr(smpp_server->database_queue_config));

found:
    octstr_destroy(p);
    gwlist_destroy(grplist, NULL);

    if (cfg_get_integer(&pool_size, grp, octstr_imm("max-connections")) == -1 || pool_size == 0)
        pool_size = 1;

    if (!(redis_host = cfg_get(grp, octstr_imm("host"))))
        panic(0, "REDIS: directive 'host' is not specified!");
    if (cfg_get_integer(&redis_port, grp, octstr_imm("port")) == -1)
        panic(0, "REDIS: directive 'port' is not specified!");
    redis_password = cfg_get(grp, octstr_imm("password"));
    cfg_get_integer(&redis_database, grp, octstr_imm("database"));
    cfg_get_integer(&redis_idle_timeout, grp, octstr_imm("idle-timeout"));

    db_conf = gw_malloc(sizeof(DBConf));
    db_conf->redis = gw_malloc(sizeof(RedisConf));
    db_conf->redis->host = redis_host;
    db_conf->redis->port = redis_port;
    db_conf->redis->password = redis_password;
    db_conf->redis->database = redis_database;
    db_conf->redis->idle_timeout = redis_idle_timeout;

    pool = dbpool_create(DBPOOL_REDIS, db_conf, pool_size);
    if (dbpool_conn_count(pool) == 0)
        panic(0, "REDIS: database pool has no connections!");

    json_set_alloc_funcs(smpp_redis_json_malloc, smpp_redis_json_free);
    smpp_redis_check_version(pool);

    smpp_database->queue_context = pool;
    smpp_database->add_message = smpp_database_redis_add_message;
    smpp_database->get_stored = smpp_database_redis_get_stored;
    smpp_database->delete = smpp_database_redis_remove;
    smpp_database->add_pdu = smpp_database_redis_add_pdu;
    smpp_database->get_stored_pdu = smpp_database_redis_get_stored_pdu;
    smpp_database->get_dlrs = smpp_database_redis_get_dlrs;
    smpp_database->delete_dlr = smpp_database_redis_remove_dlr;
    smpp_database->get_esmes_with_queued = smpp_database_redis_get_esmes_with_queued;

    info(0, "REDIS: queue backend attached for id '%s'", octstr_get_cstr(smpp_server->database_queue_config));
    return 1;
}

#else /* !HAVE_REDIS */

int smpp_database_redis_queue_attach(SMPPServer *smpp_server, SMPPDatabase *smpp_database)
{
    (void) smpp_database;
    panic(0, "REDIS: database-queue-type=redis but Kannel was built without --with-redis (id '%s')",
            octstr_get_cstr(smpp_server->database_queue_config));
    return 0;
}

#endif /* HAVE_REDIS */
