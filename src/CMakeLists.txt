# vdb.so: the four VDB plug-ins for mysqld 8.4 (implementation plan doc 08).
# Built inside MySQL's source tree as storage/vdb.
DISABLE_MISSING_PROFILE_WARNING()
ADD_DEFINITIONS(-DMYSQL_SERVER)

# The protocol codec, generated from vdb.proto with MySQL's bundled protoc
# and linked against its libprotobuf-lite (D-81).
FILE(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
MYSQL_PROTOBUF_GENERATE_CPP_LIBRARY(vdb_protocol_lite
  ${CMAKE_CURRENT_SOURCE_DIR}/common/vdb.proto
  OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
TARGET_LINK_LIBRARIES(vdb_protocol_lite PUBLIC ext::libprotobuf-lite)
SET_TARGET_PROPERTIES(vdb_protocol_lite PROPERTIES POSITION_INDEPENDENT_CODE ON)

SET(VDB_SOURCES
  plugin.cc
  common/conn.cc
  common/proto.cc
  common/errors.cc
  common/session.cc
  storage/ha_vdb.cc
  storage/hton.cc
  storage/scan.cc
  storage/interleave.cc
  storage/mrr.cc
  storage/rows.cc
  storage/keys.cc
  storage/cond.cc
  storage/write.cc
  storage/ddl.cc
  storage/stats.cc
  storage/lockwait.cc
  storage/images.cc
  auth/relay.cc
  auth/caching_sha2.cc
  auth/native.cc
  statements/audit.cc
  statements/preparse.cc
  statements/querystart.cc
  statements/queryend.cc
  statements/connection.cc
  statements/eventpool.cc
)

MYSQL_ADD_PLUGIN(vdb ${VDB_SOURCES}
  STORAGE_ENGINE MODULE_ONLY MODULE_OUTPUT_NAME "vdb"
  LINK_LIBRARIES vdb_protocol_lite extra::rapidjson)
IF(TARGET vdb)
  TARGET_INCLUDE_DIRECTORIES(vdb SYSTEM BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
  STRING(REPLACE " " ";" vdb_pb_flags "${MY_PUBLIC_PROTOBUF_FLAGS}")
  TARGET_COMPILE_OPTIONS(vdb PRIVATE ${vdb_pb_flags})
ENDIF()
