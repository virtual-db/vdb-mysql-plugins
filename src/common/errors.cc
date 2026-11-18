/* errors.cc: the one table from error codes to HA_ERR_* (product doc 04 §6). */
#include "storage/vdb/common/errors.h"

#include "my_base.h"

int vdb_ha_error(const vdb::Error &e, std::string *msg, unsigned *errkey) {
  *msg = e.message();
  switch (e.code()) {
    case vdb_proto::E_DUPLICATE_KEY:
      if (errkey) *errkey = e.index();
      return HA_ERR_FOUND_DUPP_KEY;
    case vdb_proto::E_NO_REFERENCED_ROW:
      *msg = e.constraint();
      return HA_ERR_NO_REFERENCED_ROW;
    case vdb_proto::E_ROW_IS_REFERENCED:
      *msg = e.constraint();
      return HA_ERR_ROW_IS_REFERENCED;
    case vdb_proto::E_FK_DEPTH:
      return HA_ERR_FK_DEPTH_EXCEEDED;
    case vdb_proto::E_LOCK_WAIT_TIMEOUT:
      return HA_ERR_LOCK_WAIT_TIMEOUT;
    case vdb_proto::E_DEADLOCK:
      return HA_ERR_LOCK_DEADLOCK;
    case vdb_proto::E_DELTA_FULL:
      return HA_ERR_RECORD_FILE_FULL;
    case vdb_proto::E_LOCK_MEMORY:
      return HA_ERR_LOCK_TABLE_FULL;
    case vdb_proto::E_KILLED:
      return HA_ERR_QUERY_INTERRUPTED;
    case vdb_proto::E_REFUSED:
    case vdb_proto::E_SOURCE:
    case vdb_proto::E_INTERNAL:
    default:
      *msg = "VDB: " + e.message();
      return HA_ERR_VDB;
  }
}

int vdb_unavailable(std::string *msg) {
  *msg = "VDB is unavailable";
  return HA_ERR_VDB;
}
