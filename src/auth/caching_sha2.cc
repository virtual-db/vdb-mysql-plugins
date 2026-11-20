/* caching_sha2.cc: vdb_mysql_auth asks the client for caching_sha2_password,
   the MySQL 8+ default (product doc 03 §3.1). */
#include "storage/vdb/auth/relay.h"

static int vdb_auth_sha2(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info) {
  return vdb_relay_authenticate(vio, info, "caching_sha2_password");
}

struct st_mysql_auth vdb_mysql_auth_handler = {MYSQL_AUTHENTICATION_INTERFACE_VERSION,
                                               "caching_sha2_password",
                                               vdb_auth_sha2,
                                               vdb_auth_generate,
                                               vdb_auth_validate,
                                               vdb_auth_set_salt,
                                               0,
                                               nullptr};
