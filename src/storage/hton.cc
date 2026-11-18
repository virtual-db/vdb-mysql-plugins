/* hton.cc: the handlerton callbacks (product doc 03 §2.3): statement and
   transaction commit and rollback, savepoints, consistent snapshots, the end
   of a connection, kill, and push_to_engine. Two-phase commit is not
   implemented (product doc 07 §5). */
#include "sql/handler.h"
#include "sql/sql_class.h"
#include "storage/vdb/common/errors.h"
#include "storage/vdb/common/session.h"
#include "storage/vdb/storage/ha_vdb.h"

namespace {

/* Send a message with an empty or named body on the session's connection.
   A transport failure means vdb-mysql is gone: the transaction is lost. */
int send_simple(THD *thd, uint8_t op, const google::protobuf::MessageLite &req) {
  Vdb_session *s = static_cast<Vdb_session *>(thd_get_ha_data(thd, vdb_hton));
  if (s == nullptr || !s->conn.is_open()) return 0;
  vdb::Error e;
  int rc = s->conn.call(op, req, nullptr, &e);
  if (rc < 0) {
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_VDB, "VDB is unavailable", "vdb_mysql_storage");
    return HA_ERR_VDB;
  }
  if (rc > 0) {
    std::string msg;
    unsigned key;
    int err = vdb_ha_error(e, &msg, &key);
    if (err == HA_ERR_LOCK_DEADLOCK || err == HA_ERR_LOCK_WAIT_TIMEOUT) return err;
    my_error(ER_GET_ERRMSG, MYF(0), err, msg.c_str(), "vdb_mysql_storage");
    return err;
  }
  return 0;
}

/* commit(all=false) ends a statement; in autocommit it ends the transaction. */
int vdb_commit(handlerton *, THD *thd, bool all) {
  Vdb_session *s = static_cast<Vdb_session *>(thd_get_ha_data(thd, vdb_hton));
  if (s == nullptr) return 0;
  const bool multi = thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
  vdb::Empty req;
  int rc;
  if (all || !multi) {
    rc = send_simple(thd, vdb_proto::OP_COMMIT, req);
    s->in_trx = false;
  } else {
    rc = send_simple(thd, vdb_proto::OP_STMT_END, req);
  }
  s->stmt_open = false;
  return rc;
}

int vdb_rollback(handlerton *, THD *thd, bool all) {
  Vdb_session *s = static_cast<Vdb_session *>(thd_get_ha_data(thd, vdb_hton));
  if (s == nullptr) return 0;
  const bool multi = thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
  vdb::Empty req;
  int rc;
  if (all || !multi) {
    rc = send_simple(thd, vdb_proto::OP_ROLLBACK, req);
    s->in_trx = false;
  } else {
    rc = send_simple(thd, vdb_proto::OP_STMT_ROLLBACK, req);
  }
  s->stmt_open = false;
  return rc;
}

/* Savepoints are identified by the address mysqld gives them. */
std::string sv_name(void *sv) { return std::to_string(reinterpret_cast<uintptr_t>(sv)); }

int vdb_savepoint_set(handlerton *, THD *thd, void *sv) {
  vdb::Savepoint m;
  m.set_name(sv_name(sv));
  return send_simple(thd, vdb_proto::OP_SAVEPOINT, m);
}

int vdb_savepoint_rollback(handlerton *, THD *thd, void *sv) {
  vdb::Savepoint m;
  m.set_name(sv_name(sv));
  return send_simple(thd, vdb_proto::OP_ROLLBACK_TO, m);
}

int vdb_savepoint_release(handlerton *, THD *thd, void *sv) {
  vdb::Savepoint m;
  m.set_name(sv_name(sv));
  return send_simple(thd, vdb_proto::OP_RELEASE, m);
}

bool vdb_savepoint_rollback_can_release_mdl(handlerton *, THD *) { return true; }

/* START TRANSACTION WITH CONSISTENT SNAPSHOT: take the snapshot pair now. */
int vdb_start_consistent_snapshot(handlerton *, THD *thd) {
  std::string msg;
  if (int e = vdb_begin_statement(thd, &msg)) {
    my_error(ER_GET_ERRMSG, MYF(0), e, msg.c_str(), "vdb_mysql_storage");
    return 1;
  }
  vdb::Empty req;
  return send_simple(thd, vdb_proto::OP_SNAPSHOT, req) ? 1 : 0;
}

/* The end of a connection: roll back and release everything. */
int vdb_close_connection(handlerton *, THD *thd) {
  vdb_free_session(thd);
  return 0;
}

/* KILL: sent on the shared kill connection, because the session's own may
   be waiting on a lock. */
Vdb_conn *kill_conn = nullptr;
mysql_mutex_t kill_mutex;
bool kill_mutex_ready = false;

void vdb_kill_connection(handlerton *, THD *thd) {
  if (!kill_mutex_ready) return;
  mysql_mutex_lock(&kill_mutex);
  std::string err;
  if (kill_conn == nullptr) kill_conn = new Vdb_conn();
  if (kill_conn->is_open() || kill_conn->connect(vdb::CONN_KILL, 0, &err)) {
    vdb::Kill k;
    k.set_conn_id(thd_get_thread_id(thd));
    kill_conn->call(vdb_proto::OP_KILL, k, nullptr, nullptr);
  }
  mysql_mutex_unlock(&kill_mutex);
}

void vdb_post_ddl(THD *) {}

}  // namespace

handler *vdb_create_handler(handlerton *hton, TABLE_SHARE *table, bool, MEM_ROOT *mem_root);
int vdb_push_to_engine(THD *thd, AccessPath *root, JOIN *join);

/* Build the handlerton (doc 08 §2). */
int vdb_storage_init(void *p) {
  vdb_hton = static_cast<handlerton *>(p);
  mysql_mutex_init(PSI_NOT_INSTRUMENTED, &kill_mutex, MY_MUTEX_INIT_FAST);
  kill_mutex_ready = true;
  vdb_hton->state = SHOW_OPTION_YES;
  vdb_hton->db_type = DB_TYPE_UNKNOWN;
  vdb_hton->create = vdb_create_handler;
  vdb_hton->commit = vdb_commit;
  vdb_hton->rollback = vdb_rollback;
  vdb_hton->savepoint_offset = 0;
  vdb_hton->savepoint_set = vdb_savepoint_set;
  vdb_hton->savepoint_rollback = vdb_savepoint_rollback;
  vdb_hton->savepoint_release = vdb_savepoint_release;
  vdb_hton->savepoint_rollback_can_release_mdl = vdb_savepoint_rollback_can_release_mdl;
  vdb_hton->start_consistent_snapshot = vdb_start_consistent_snapshot;
  vdb_hton->close_connection = vdb_close_connection;
  vdb_hton->kill_connection = vdb_kill_connection;
  vdb_hton->push_to_engine = vdb_push_to_engine;
  vdb_hton->post_ddl = vdb_post_ddl;
  vdb_hton->flags = HTON_SUPPORTS_FOREIGN_KEYS | HTON_SUPPORTS_ATOMIC_DDL;
  return 0;
}

int vdb_storage_deinit(void *) {
  if (kill_mutex_ready) {
    mysql_mutex_lock(&kill_mutex);
    delete kill_conn;
    kill_conn = nullptr;
    mysql_mutex_unlock(&kill_mutex);
    mysql_mutex_destroy(&kill_mutex);
    kill_mutex_ready = false;
  }
  return 0;
}
