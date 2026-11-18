/* session.cc: per-THD state, the data connection, and StmtBegin with the
   session's identity, isolation and evaluation context (product doc 03 §2.8,
   implementation plan doc 08 §3.2, §3.3). */
#include "storage/vdb/common/session.h"

#include "mysql/plugin.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/handler.h"
#include "sql/set_var.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/system_variables.h"
#include "sql/transaction.h"
#include "sql/tztime.h"
#include "storage/vdb/common/errors.h"

handlerton *vdb_hton = nullptr;

Vdb_session *vdb_get_session(THD *thd) {
  auto *s = static_cast<Vdb_session *>(thd_get_ha_data(thd, vdb_hton));
  if (s == nullptr) {
    s = new Vdb_session();
    thd_set_ha_data(thd, vdb_hton, s);
  }
  return s;
}

void vdb_free_session(THD *thd) {
  auto *s = static_cast<Vdb_session *>(thd_get_ha_data(thd, vdb_hton));
  if (s == nullptr) return;
  if (s->conn.is_open()) {
    vdb::Empty req;
    s->conn.call(vdb_proto::OP_SESSION_END, req, nullptr, nullptr);
  }
  delete s;
  thd_set_ha_data(thd, vdb_hton, nullptr);
}

Vdb_conn *vdb_data_conn(THD *thd, std::string *err) {
  Vdb_session *s = vdb_get_session(thd);
  if (!s->conn.is_open()) {
    s->stmt_open = false;
    if (!s->conn.connect(vdb::CONN_DATA, thd_get_thread_id(thd), err)) return nullptr;
  }
  return &s->conn;
}

bool vdb_is_control(THD *thd) {
  const Security_context *sc = thd->security_context();
  if (sc == nullptr) return false;
  LEX_CSTRING u = sc->priv_user(), h = sc->priv_host();
  return u.length == 9 && memcmp(u.str, "vdb_admin", 9) == 0 && h.length == 9 && memcmp(h.str, "localhost", 9) == 0;
}

static vdb::Isolation isolation_of(THD *thd) {
  switch (thd_tx_isolation(thd)) {
    case ISO_READ_UNCOMMITTED:
      return vdb::READ_UNCOMMITTED;
    case ISO_READ_COMMITTED:
      return vdb::READ_COMMITTED;
    case ISO_SERIALIZABLE:
      return vdb::SERIALIZABLE;
    default:
      return vdb::REPEATABLE_READ;
  }
}

static void add_setting(vdb::StmtBegin *m, const char *name, const char *value, size_t len) {
  vdb::Setting *s = m->add_eval();
  s->set_name(name);
  s->set_value(value, len);
}

int vdb_begin_statement(THD *thd, std::string *msg) {
  Vdb_session *s = vdb_get_session(thd);
  Vdb_conn *c = vdb_data_conn(thd, msg);
  if (c == nullptr) return HA_ERR_VDB;
  const bool multi = thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
  if (!s->stmt_open || s->stmt_query_id != static_cast<uint64_t>(thd->query_id)) {
    vdb::StmtBegin m;
    const Security_context *sc = thd->security_context();
    LEX_CSTRING u = sc->priv_user(), h = sc->priv_host();
    m.set_account(std::string("'") + std::string(u.str, u.length) + "'@'" + std::string(h.str, h.length) + "'");
    if (thd->db().str) m.set_database(thd->db().str, thd->db().length);
    m.set_control(vdb_is_control(thd));
    m.set_isolation(isolation_of(thd));
    m.set_autocommit(!multi);
    m.set_foreign_key_checks(!(thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS));
    m.set_lock_wait_ms(0);  // the core's default: InnoDB's 50 seconds
    m.set_bka(thd->optimizer_switch_flag(OPTIMIZER_SWITCH_BKA));
    m.set_read_only(thd->tx_read_only);
    const String *tz = thd->variables.time_zone->get_name();
    add_setting(&m, "time_zone", tz->ptr(), tz->length());
    LEX_STRING mode;
    if (!sql_mode_string_representation(thd, thd->variables.sql_mode, &mode))
      add_setting(&m, "sql_mode", mode.str, mode.length);
    std::string dpi = std::to_string(thd->variables.div_precincrement);
    add_setting(&m, "div_precision_increment", dpi.data(), dpi.size());
    const CHARSET_INFO *cc = thd->variables.collation_connection;
    add_setting(&m, "collation_connection", cc->m_coll_name, strlen(cc->m_coll_name));
    add_setting(&m, "character_set_connection", cc->csname, strlen(cc->csname));
    vdb::Error e;
    int rc = c->call(vdb_proto::OP_STMT_BEGIN, m, nullptr, &e);
    if (rc < 0) return vdb_unavailable(msg);
    if (rc > 0) return vdb_ha_error(e, msg, nullptr);
    s->stmt_open = true;
    s->stmt_query_id = static_cast<uint64_t>(thd->query_id);
  }
  trans_register_ha(thd, false, vdb_hton, nullptr);
  if (multi) {
    trans_register_ha(thd, true, vdb_hton, nullptr);
    s->in_trx = true;
  }
  return 0;
}
