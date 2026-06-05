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

#ifndef SMPP_DATABASE_H
#define SMPP_DATABASE_H

#include "gwlib/gwlib.h"
#include "gwlib/dbpool.h"
#include "gw/msg.h"
#include "gw/load.h"
#include "smpp_server.h"
#include "smpp_bearerbox.h"
#include "smpp_esme.h"
#include "smpp_queued_pdu.h"

#define SMPP_DATABASE_BATCH_LIMIT 1000

/* Which physical store table / Redis key prefix to use for message queue I/O */
#define SMPP_DATABASE_STORE_PRIMARY         0  /* database-store-table (store-primary MT, MO) */
#define SMPP_DATABASE_STORE_BEARERBOX_QUEUE 1  /* database-queue-store-table (SMSC/bearerbox fallback) */
#define SMPP_DATABASE_STORE_AUTO           -1  /* derive table from sms_type / store-primary flags */

#ifdef __cplusplus
extern "C" {
#endif
    typedef struct {
        Msg *msg;
        unsigned long global_id;
        SMPPServer *smpp_server;
        long wakeup_thread_id;
        Octstr *store_table;
    } SMPPDatabaseMsg;
    
    
    
    typedef struct SMPPDatabase {
        SMPPESMEAuthResult *(*authenticate) (void *context, Octstr *system_id, Octstr *password);
        int (*add_message)(SMPPServer *context, Msg *msg, int store_kind);
        int (*add_pdu)(SMPPServer *context, SMPPQueuedPDU *smpp_queued_pdu);      
        List *(*get_stored)(SMPPServer *context, long sms_type, Octstr *service, long limit, int store_kind);
        List *(*get_dlrs)(SMPPServer *context, Octstr *service, long limit);
        List *(*get_stored_pdu)(SMPPServer *context, Octstr *service, long limit);
        List *(*get_routes)(SMPPServer *context, int direction, Octstr *service);
        int (*deduct_credit)(SMPPServer *context, Octstr *service, double value);
        int (*delete)(SMPPServer *context, unsigned long global_id, int temporary);
        int (*delete_dlr)(SMPPServer *context, unsigned long global_id);
        List *(*get_esmes_with_queued)(SMPPServer *smpp_server);
        void (*shutdown)(SMPPServer *context);
        void *context;
        void *queue_context;
        Dict *pending_pdu;
        Dict *pending_msg;
        Dict *pending_msg_store;
        enum db_type sql_dialect;
        
    } SMPPDatabase;
    
    
    SMPPDatabaseMsg *smpp_database_msg_create();
    void smpp_database_msg_destroy();

    SMPPDatabase *smpp_database_create();
    void smpp_database_destroy(SMPPDatabase *smpp_database);
            

    void *smpp_database_init(SMPPServer *smpp_server);
    void smpp_database_shutdown(SMPPServer *smpp_server);
    
    void *smpp_database_mysql_init(SMPPServer *smpp_server);
#ifdef HAVE_PGSQL
    void *smpp_database_pgsql_init(SMPPServer *smpp_server);
#endif
    int smpp_database_redis_queue_attach(SMPPServer *smpp_server, SMPPDatabase *smpp_database);
    
    SMPPESMEAuthResult *smpp_database_auth(SMPPServer *smpp_server, Octstr *username, Octstr *password);
    
    Octstr *smpp_database_store_table_name(SMPPServer *smpp_server, int store_kind);
    Octstr *smpp_database_get_stored_table_name(SMPPServer *smpp_server, long sms_type);
    
    int smpp_database_add_message(SMPPServer *smpp_server, Msg *msg);
    int smpp_database_add_queue_message(SMPPServer *smpp_server, Msg *msg);
    int smpp_database_add_pdu(SMPPServer *smpp_server, SMPPQueuedPDU *smpp_queued_pdu);
    List *smpp_database_get_stored(SMPPServer *smpp_server, long sms_type, Octstr *service, long limit);
    List *smpp_database_get_queue_stored(SMPPServer *smpp_server, long sms_type, Octstr *service, long limit);
    List *smpp_database_get_dlrs(SMPPServer *smpp_server, Octstr *service, long limit);
    List *smpp_database_get_stored_pdu(SMPPServer *smpp_server, Octstr *service, long limit);
    List *smpp_database_get_routes(SMPPServer *smpp_server, int direction, Octstr *service);
    int smpp_database_deduct_credit(SMPPServer *smpp_server, Octstr *service, double value);
    List *smpp_database_get_esmes_with_queued(SMPPServer *smpp_server);
    
    int smpp_database_remove(SMPPServer *smpp_server, unsigned long global_id, int temporary);
    int smpp_database_remove_dlr(SMPPServer *smpp_server, unsigned long global_id);


#ifdef __cplusplus
}
#endif

#endif /* SMPP_DATABASE_H */

