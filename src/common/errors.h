/* errors.h: an error reply to HA_ERR_* and its message (doc 07 §4). */
#ifndef VDB_COMMON_ERRORS_H
#define VDB_COMMON_ERRORS_H

#include <string>

#include "storage/vdb/common/proto.h"

/* HA_ERR_VDB is returned for VDB's own refusals; handler::print_error
   reports it with the engine's message from get_error_message. */
constexpr int HA_ERR_VDB = 1000 + 190;

/* Map an error reply to a handler error code. *msg receives the message to
   report through get_error_message; *errkey the duplicate key's index. */
int vdb_ha_error(const vdb::Error &e, std::string *msg, unsigned *errkey);

/* The error for a failed connection. */
int vdb_unavailable(std::string *msg);

#endif
