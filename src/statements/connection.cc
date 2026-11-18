/* connection.cc: MYSQL_AUDIT_CONNECTION_CONNECT and _DISCONNECT become
   Events, asynchronously (doc 08 §5). */
#include "storage/vdb/statements/statements.h"

int vdb_on_connection(MYSQL_THD, const mysql_event_connection *ev) {
  if (ev->event_subclass != MYSQL_AUDIT_CONNECTION_CONNECT && ev->event_subclass != MYSQL_AUDIT_CONNECTION_DISCONNECT)
    return 0;
  if (ev->priv_user.length == 9 && memcmp(ev->priv_user.str, "vdb_admin", 9) == 0) return 0;
  vdb::Event e;
  e.set_kind(ev->event_subclass == MYSQL_AUDIT_CONNECTION_CONNECT ? vdb::EVENT_CONNECT : vdb::EVENT_DISCONNECT);
  e.set_conn_id(ev->connection_id);
  if (ev->priv_user.str) e.set_account(ev->priv_user.str, ev->priv_user.length);
  if (ev->host.str && ev->host.length)
    e.set_host(ev->host.str, ev->host.length);
  else if (ev->ip.str)
    e.set_host(ev->ip.str, ev->ip.length);
  e.set_error_code(static_cast<uint32_t>(ev->status));
  vdb_event_send(e);
  return 0;
}
