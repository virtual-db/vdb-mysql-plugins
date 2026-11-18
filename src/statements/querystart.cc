/* querystart.cc: MYSQL_AUDIT_QUERY_START (doc 08 §5): refuse XA statements
   and CREATE EVENT, which privileges alone cannot stop; for view, routine and
   trigger DDL, make sure vdb-mysql is reachable, because the statement must
   be recorded when it succeeds (product doc 09 §3.3). Fail closed. */
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/sql_class.h"
#include "storage/vdb/statements/statements.h"

bool vdb_is_object_ddl(enum_sql_command c) {
  switch (c) {
    case SQLCOM_CREATE_VIEW:
    case SQLCOM_DROP_VIEW:
    case SQLCOM_CREATE_PROCEDURE:
    case SQLCOM_ALTER_PROCEDURE:
    case SQLCOM_DROP_PROCEDURE:
    case SQLCOM_CREATE_SPFUNCTION:
    case SQLCOM_ALTER_FUNCTION:
    case SQLCOM_DROP_FUNCTION:
    case SQLCOM_CREATE_TRIGGER:
    case SQLCOM_DROP_TRIGGER:
      return true;
    default:
      return false;
  }
}

int vdb_on_query_start(MYSQL_THD thd, const mysql_event_query *ev) {
  if (vdb_statements_skip(thd)) return 0;
  switch (ev->sql_command_id) {
    case SQLCOM_XA_START:
    case SQLCOM_XA_END:
    case SQLCOM_XA_PREPARE:
    case SQLCOM_XA_COMMIT:
    case SQLCOM_XA_ROLLBACK:
    case SQLCOM_XA_RECOVER:
      my_printf_error(ER_UNKNOWN_ERROR, "VDB: XA transactions are not supported", MYF(0));
      return 1;
    case SQLCOM_CREATE_EVENT:
    case SQLCOM_ALTER_EVENT:
      my_printf_error(ER_UNKNOWN_ERROR, "VDB: events are not supported; the event scheduler is off", MYF(0));
      return 1;
    default:
      break;
  }
  if (vdb_is_object_ddl(ev->sql_command_id)) {
    vdb::Empty ping;
    if (vdb_event_call(vdb_proto::OP_PING, ping, nullptr, nullptr, nullptr) != 0) {
      my_printf_error(ER_UNKNOWN_ERROR, "VDB is unavailable", MYF(0));
      return 1;
    }
  }
  return 0;
}
