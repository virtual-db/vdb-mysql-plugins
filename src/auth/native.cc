/* native.cc: vdb_mysql_auth_native asks the client for
   mysql_native_password, the RDS for MySQL 8.0 default (product doc 03 §3.1). */
#include "storage/vdb/auth/relay.h"

static int vdb_auth_native(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info) {
  return vdb_relay_authenticate(vio, info, "mysql_native_password");
}

struct st_mysql_auth vdb_mysql_auth_native_handler = {MYSQL_AUTHENTICATION_INTERFACE_VERSION,
                                                      "mysql_native_password",
                                                      vdb_auth_native,
                                                      vdb_auth_generate,
                                                      vdb_auth_validate,
                                                      vdb_auth_set_salt,
                                                      0,
                                                      nullptr};
