/* queryend.cc: MYSQL_AUDIT_QUERY_STATUS_END (doc 08 §5): successful view,
   routine and trigger DDL is recorded in the catalog of record before the
   client sees success (DdlCaptured, synchronous); every statement becomes
   an Event, asynchronously. */
#include "my_systime.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/set_var.h"
#include "sql/sp_head.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/table.h"
#include "storage/vdb/statements/statements.h"

bool vdb_is_object_ddl(enum_sql_command c);

namespace {
std::string account_of(THD *thd) {
  const Security_context *sc = thd->security_context();
  LEX_CSTRING u = sc->priv_user(), h = sc->priv_host();
  return "'" + std::string(u.str, u.length) + "'@'" + std::string(h.str, h.length) + "'";
}

std::string object_name(THD *thd) {
  LEX *lex = thd->lex;
  if (lex == nullptr) return "";
  if (lex->spname != nullptr)
    return std::string(lex->spname->m_db.str, lex->spname->m_db.length) + "." +
           std::string(lex->spname->m_name.str, lex->spname->m_name.length);
  if (lex->query_tables != nullptr)
    return std::string(lex->query_tables->db) + "." + std::string(lex->query_tables->table_name);
  return "";
}
}  // namespace

int vdb_on_query_end(MYSQL_THD thd, const mysql_event_query *ev) {
  if (vdb_statements_skip(thd)) return 0;
  if (ev->status == 0 && vdb_is_object_ddl(ev->sql_command_id)) {
    vdb::DdlCaptured m;
    m.set_conn_id(ev->connection_id);
    m.set_statement(ev->query.str, ev->query.length);
    if (thd->db().str) m.set_database(thd->db().str, thd->db().length);
    LEX_STRING mode;
    if (!sql_mode_string_representation(thd, thd->variables.sql_mode, &mode)) m.set_sql_mode(mode.str, mode.length);
    m.set_definer(account_of(thd));
    m.set_kind(static_cast<uint32_t>(ev->sql_command_id));
    m.set_object_name(object_name(thd));
    vdb::Error e;
    if (vdb_event_call(vdb_proto::OP_DDL_CAPTURED, m, nullptr, &e, nullptr) != 0) {
      my_printf_error(ER_UNKNOWN_ERROR, "VDB: the statement ran but could not be recorded: %s", MYF(0),
                      e.message().c_str());
      return 1;
    }
  }
  vdb::Event e;
  e.set_kind(vdb::EVENT_STATEMENT);
  e.set_conn_id(ev->connection_id);
  e.set_account(account_of(thd));
  e.set_query(ev->query.str, ev->query.length);
  if (thd->db().str) e.set_database(thd->db().str, thd->db().length);
  ulonglong now = my_micro_time();
  if (now > thd->start_utime) e.set_duration_us(now - thd->start_utime);
  longlong rows = thd->get_row_count_func();
  e.set_rows(rows > 0 ? static_cast<uint64_t>(rows) : 0);
  e.set_rows_sent(thd->get_sent_row_count());
  e.set_error_code(static_cast<uint32_t>(ev->status));
  vdb_event_send(e);
  return 0;
}
