# Redis queue key layout (KSMPPD hybrid mode)

When `database-queue-type=redis` is set, the `database-*-table` directives name Redis key prefixes (not MySQL tables) for queue data. MySQL still holds `smpp_user`, `smpp_route`, and schema version tables.

Requires **Redis 6.2+** (`LPOP key count`).

## Message stores

Two separate store pools may be configured:

| Config | Default prefix | Purpose |
|--------|----------------|---------|
| `database-store-table` | `smpp_store` | Store-primary MT (`database-store-primary=1`), MO when ESME has no receivers |
| `database-queue-store-table` | `smpp_store_queue` | Bearerbox/SMSC fallback when no bearerbox is online (`database-enable-queue=1`) |

Each prefix uses the same key layout:

| Key | Type | Purpose |
|-----|------|---------|
| `{prefix}:id` | string (INCR) | Auto-increment `global_id` |
| `{prefix}:queue` | list | All queued messages (JSON) |
| `{prefix}:esmes` | set | ESMEs with pending store data (poll index) |

**Store-primary MT (external pickup):**

```bash
redis-cli LPOP smpp_store:queue 10
```

**Bearerbox fallback queue (ksmppd drains on reconnect):**

```bash
redis-cli LPOP smpp_store_queue:queue 10
```

**Enqueue (ksmppd or external):**

```bash
redis-cli LPUSH smpp_store:queue '{"global_id":1,"service":"myesme","sms_type":2,...}'
redis-cli LPUSH smpp_store_queue:queue '{"global_id":1,"service":"myesme","sms_type":2,...}'
```

Each JSON blob includes `service`, `sms_type`, and all SMS fields. ksmppd filters in-process when callers pass `service` and/or `sms_type` to `get_stored`; non-matching popped entries are pushed back to the same list.

**JSON fields:** All SMS `Msg` fields plus `global_id`. Binary `msgdata`/`udhdata` are URL-encoded (`%XX`).

## PDU queue (`database-pdu-table`, default `smpp_queued_pdu`)

| Key | Type | Purpose |
|-----|------|---------|
| `smpp_queued_pdu:id` | string (INCR) | PDU row id |
| `smpp_queued_pdu:{system_id}` | list | Queued PDUs (JSON) |
| `smpp_queued_pdu:esmes` | set | ESMEs with pending PDUs |

**JSON:** `global_id`, `time`, `system_id`, `pdu` (URL-encoded packed PDU without command length).

## DLR queue (`database-dlr-table`, default `smpp_dlr`)

| Key | Type | Purpose |
|-----|------|---------|
| `smpp_dlr:id` | string (INCR) | Optional id assignment |
| `smpp_dlr:{service}` | list | Pending DLR records (JSON) |
| `smpp_dlr:esmes` | set | ESMEs with pending DLRs |

**External DLR injection (store-primary mode):**

```bash
redis-cli SADD smpp_dlr:esmes myesme
redis-cli LPUSH smpp_dlr:myesme '{"global_id":0,"message_id":"abc123","service":"myesme","status":"DELIVRD","err_code":0,"submit_date":"2401011200","done_date":"2401011201","destination_addr":"1234","source_addr":"5678","smsc_id":"smsc1","text":""}'
```

If `global_id` is `0` or omitted, ksmppd assigns one via `INCR smpp_dlr:id` when processing.

## Service name normalization

Empty service is treated as `_global_` for index sets. Service comparisons are case-insensitive.

## Claim semantics

`LPOP` removes items from the list immediately. Failed delivery with temporary remove requeues via `LPUSH` using the stored JSON blob.
