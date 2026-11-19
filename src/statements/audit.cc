/* audit.cc: the statements plug-in's class mask and event dispatch
   (implementation plan doc 08 §5, D-47). It is an audit plug-in in MySQL's
   terms; VDB calls it the statements plug-in because it acts on statements. */
#include "storage/vdb/statements/statements.h"

#include "sql/sql_class.h"
#include "storage/vdb/common/session.h"

bool vdb_statements_skip(MYSQL_THD thd) { return thd == nullptr || vdb_is_control(thd); }

static int vdb_event_notify(MYSQL_THD thd, mysql_event_class_t cls, const void *ev) {
  switch (cls) {
    case MYSQL_AUDIT_PARSE_CLASS: {
      auto *p = static_cast<const mysql_event_parse *>(ev);
      return p->event_subclass == MYSQL_AUDIT_PARSE_PREPARSE ? vdb_on_preparse(thd, p) : 0;
    }
    case MYSQL_AUDIT_QUERY_CLASS: {
      auto *q = static_cast<const mysql_event_query *>(ev);
      if (q->event_subclass == MYSQL_AUDIT_QUERY_START) return vdb_on_query_start(thd, q);
      if (q->event_subclass == MYSQL_AUDIT_QUERY_STATUS_END) return vdb_on_query_end(thd, q);
      return 0;
    }
    case MYSQL_AUDIT_CONNECTION_CLASS:
      return vdb_on_connection(thd, static_cast<const mysql_event_connection *>(ev));
    default:
      return 0;
  }
}

struct st_mysql_audit vdb_statements_descriptor = {
    MYSQL_AUDIT_INTERFACE_VERSION,
    nullptr,
    vdb_event_notify,
    {
        0,                                                                                              /* GENERAL */
        static_cast<unsigned long>(MYSQL_AUDIT_CONNECTION_CONNECT | MYSQL_AUDIT_CONNECTION_DISCONNECT), /* CONNECTION */
        static_cast<unsigned long>(MYSQL_AUDIT_PARSE_PREPARSE),                                         /* PARSE */
        0,                                                                                  /* AUTHORIZATION */
        0,                                                                                  /* TABLE_ACCESS */
        0,                                                                                  /* GLOBAL_VARIABLE */
        0,                                                                                  /* SERVER_STARTUP */
        0,                                                                                  /* SERVER_SHUTDOWN */
        0,                                                                                  /* COMMAND */
        static_cast<unsigned long>(MYSQL_AUDIT_QUERY_START | MYSQL_AUDIT_QUERY_STATUS_END), /* QUERY */
        0,                                                                                  /* STORED_PROGRAM */
        0,                                                                                  /* AUTHENTICATION */
        0,                                                                                  /* MESSAGE */
    }};

int vdb_statements_init(void *) {
  vdb_events_start();
  return 0;
}

int vdb_statements_deinit(void *) {
  vdb_events_stop();
  return 0;
}
