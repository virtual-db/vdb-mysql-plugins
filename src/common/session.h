/* session.h: per-THD state of the storage engine (doc 08 §3.2): the data
   connection, whether StmtBegin was sent for the current statement, and the
   transaction registration. Kept in the handlerton's per-session slot. */
#ifndef VDB_COMMON_SESSION_H
#define VDB_COMMON_SESSION_H

#include <cstdint>
#include <string>

#include "storage/vdb/common/conn.h"

class THD;
struct handlerton;

struct Vdb_session {
  Vdb_conn conn;
  bool stmt_open = false;      // StmtBegin sent for the current statement
  uint64_t stmt_query_id = 0;  // the statement it was sent for
  bool in_trx = false;         // registered for the transaction
};

extern handlerton *vdb_hton;

/* The session of thd, created on first use. */
Vdb_session *vdb_get_session(THD *thd);

/* The session's data connection, connected on first use. Returns nullptr
   with *err set when vdb-mysql is unreachable. */
Vdb_conn *vdb_data_conn(THD *thd, std::string *err);

/* Send StmtBegin once per statement and register with the transaction
   coordinator. Returns a handler error code or 0. */
int vdb_begin_statement(THD *thd, std::string *msg);

/* Free the session (close_connection). */
void vdb_free_session(THD *thd);

/* Whether thd is a control session (vdb_admin@localhost). */
bool vdb_is_control(THD *thd);

#endif
