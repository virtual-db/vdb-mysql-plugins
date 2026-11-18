/* plugin.cc: the four plug-ins of vdb.so (implementation plan doc 08 §2,
   D-47): vdb_mysql_storage, vdb_mysql_auth, vdb_mysql_auth_native and
   vdb_mysql_statements, with their system and status variables. The
   supervisor checks all four are ACTIVE before the client port opens. */
#include "mysql/plugin.h"
#include "mysql/plugin_audit.h"
#include "mysql/plugin_auth.h"
#include "sql/handler.h"
#include "storage/vdb/common/conn.h"
#include "storage/vdb/statements/statements.h"
#include "storage/vdb/storage/ha_vdb.h"

int vdb_storage_init(void *p);
int vdb_storage_deinit(void *p);
int vdb_statements_init(void *p);
int vdb_statements_deinit(void *p);
extern struct st_mysql_auth vdb_mysql_auth_handler;
extern struct st_mysql_auth vdb_mysql_auth_native_handler;
extern struct st_mysql_audit vdb_statements_descriptor;

static struct st_mysql_storage_engine vdb_storage_engine = {MYSQL_HANDLERTON_INTERFACE_VERSION};

static MYSQL_SYSVAR_STR(socket, vdb_socket_path, PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG,
                        "The plug-in protocol socket of vdb-mysql", nullptr, nullptr, "/run/vdb/vdb.sock");

static SYS_VAR *vdb_storage_vars[] = {MYSQL_SYSVAR(socket), nullptr};

#define VDB_STAT(n) {"vdb_" #n, reinterpret_cast<char *>(&vdb_st_##n), SHOW_LONGLONG, SHOW_SCOPE_GLOBAL}
static SHOW_VAR vdb_storage_status[] = {VDB_STAT(requests),
                                        VDB_STAT(rows_received),
                                        VDB_STAT(rows_sent),
                                        VDB_STAT(bytes_received),
                                        VDB_STAT(lock_waits),
                                        VDB_STAT(order_violations),
                                        {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};
static SHOW_VAR vdb_statements_status[] = {VDB_STAT(events_dropped),
                                           VDB_STAT(rewrites),
                                           {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

mysql_declare_plugin(vdb){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &vdb_storage_engine,
    "vdb_mysql_storage",
    "VirtualDB",
    "VDB storage engine: tables merged from the Source and the Delta",
    PLUGIN_LICENSE_GPL,
    vdb_storage_init,
    nullptr,
    vdb_storage_deinit,
    0x0100,
    vdb_storage_status,
    vdb_storage_vars,
    nullptr,
    0,
},
    {
        MYSQL_AUTHENTICATION_PLUGIN,
        &vdb_mysql_auth_handler,
        "vdb_mysql_auth",
        "VirtualDB",
        "Log in with Source credentials (caching_sha2_password clients)",
        PLUGIN_LICENSE_GPL,
        nullptr,
        nullptr,
        nullptr,
        0x0100,
        nullptr,
        nullptr,
        nullptr,
        0,
    },
    {
        MYSQL_AUTHENTICATION_PLUGIN,
        &vdb_mysql_auth_native_handler,
        "vdb_mysql_auth_native",
        "VirtualDB",
        "Log in with Source credentials (mysql_native_password clients)",
        PLUGIN_LICENSE_GPL,
        nullptr,
        nullptr,
        nullptr,
        0x0100,
        nullptr,
        nullptr,
        nullptr,
        0,
    },
    {
        MYSQL_AUDIT_PLUGIN,
        &vdb_statements_descriptor,
        "vdb_mysql_statements",
        "VirtualDB",
        "VDB statements: engine clauses, captured DDL, statement and connection events",
        PLUGIN_LICENSE_GPL,
        vdb_statements_init,
        nullptr,
        vdb_statements_deinit,
        0x0100,
        vdb_statements_status,
        nullptr,
        nullptr,
        0,
    } mysql_declare_plugin_end;
