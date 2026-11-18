/* statements.h: the statements plug-in's shared declarations (doc 08 §5). */
#ifndef VDB_STATEMENTS_H
#define VDB_STATEMENTS_H

#include "mysql/plugin.h"
#include "mysql/plugin_audit.h"
#include "storage/vdb/common/conn.h"

/* Event connections (eventpool.cc). A synchronous call borrows one of four
   connections; an asynchronous Event goes to a bounded queue and is dropped
   and counted when the queue is full (product doc 04 §2.3). */
int vdb_event_call(uint8_t op, const google::protobuf::MessageLite &req, google::protobuf::MessageLite *reply,
                   vdb::Error *err, bool *query_intercept);
void vdb_event_send(const vdb::Event &e);
void vdb_events_start();
void vdb_events_stop();
extern unsigned long long vdb_st_events_dropped, vdb_st_rewrites;

/* The handlers of each audit event class. */
int vdb_on_preparse(MYSQL_THD thd, const mysql_event_parse *ev);
int vdb_on_query_start(MYSQL_THD thd, const mysql_event_query *ev);
int vdb_on_query_end(MYSQL_THD thd, const mysql_event_query *ev);
int vdb_on_connection(MYSQL_THD thd, const mysql_event_connection *ev);

/* Whether thd is a control session, which the plug-in skips. */
bool vdb_statements_skip(MYSQL_THD thd);

#endif
