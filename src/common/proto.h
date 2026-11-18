/* proto.h: the frame and op codes of the plug-in protocol (implementation
   plan doc 07 §1, §3). Bodies are Protocol Buffers messages generated from
   vdb.proto (D-81); this file only adds the op code and the frame. It must
   match vdb-mysql/protocol/ops.go and frame.go. */
#ifndef VDB_COMMON_PROTO_H
#define VDB_COMMON_PROTO_H

#include <cstdint>
#include <string>

#include "vdb.pb.h"

namespace vdb_proto {

/* The protocol version this library speaks. */
constexpr uint32_t kVersion = 1;
/* The largest body accepted: 64 MiB. */
constexpr uint32_t kMaxBody = 64u << 20;

enum Op : uint8_t {
  OP_HELLO = 0x01,
  OP_TABLE_OPEN = 0x10,
  OP_STMT_BEGIN = 0x11,
  OP_STMT_END = 0x12,
  OP_STMT_ROLLBACK = 0x13,
  OP_COMMIT = 0x14,
  OP_ROLLBACK = 0x15,
  OP_SAVEPOINT = 0x16,
  OP_ROLLBACK_TO = 0x17,
  OP_RELEASE = 0x18,
  OP_SNAPSHOT = 0x19,
  OP_SCAN_OPEN = 0x20,
  OP_SCAN_NEXT = 0x21,
  OP_SCAN_CLOSE = 0x22,
  OP_LOOKUP = 0x23,
  OP_LOOKUP_NEXT = 0x24,
  OP_COUNT = 0x25,
  OP_STATS = 0x26,
  OP_INSERT = 0x30,
  OP_INSERT_BATCH = 0x31,
  OP_UPDATE = 0x32,
  OP_DELETE = 0x33,
  OP_AUTO_INCREMENT = 0x34,
  OP_TRUNCATE = 0x35,
  OP_CATALOG = 0x40,
  OP_ALTER = 0x41,
  OP_ALTER_ROWS = 0x42,
  OP_ALTER_PUT = 0x43,
  OP_SESSION_END = 0x50,
  OP_KILL = 0x51,
  OP_AUTH_BEGIN = 0x60,
  OP_AUTH_ANSWER = 0x61,
  OP_AUTH_PASS = 0x62,
  OP_AUTH_ABORT = 0x63,
  OP_REWRITE = 0x70,
  OP_PING = 0x71,
  OP_DDL_CAPTURED = 0x72,
  OP_EVENT = 0x73,
  OP_QUERY_RECEIVED = 0x74,
  OP_IMAGES_NEEDED = 0xFD,
  OP_WAITING = 0xFE,
  OP_ERROR = 0xFF,
};

constexpr uint8_t kReplyBit = 0x80;

/* Error codes: the core's (types.ErrorCode) and the protocol's. */
enum Code : uint32_t {
  E_DUPLICATE_KEY = 1,
  E_NO_REFERENCED_ROW = 2,
  E_ROW_IS_REFERENCED = 3,
  E_FK_DEPTH = 4,
  E_LOCK_WAIT_TIMEOUT = 5,
  E_DEADLOCK = 6,
  E_DELTA_FULL = 7,
  E_LOCK_MEMORY = 8,
  E_REFUSED = 9,
  E_SOURCE = 10,
  E_KILLED = 11,
  E_INTERNAL = 12,
  E_PROTOCOL = 1000,
  E_VERSION = 1001,
  E_UNAVAILABLE = 1002,
};

/* Write one frame to fd: op, little-endian length, body. Returns false on
   any write error. */
bool write_frame(int fd, uint8_t op, const std::string &body);

/* Read one frame from fd. Returns false on any read error or oversize body. */
bool read_frame(int fd, uint8_t *op, std::string *body);

/* The text-value encoding of keys and Source keys: per value a 4-byte
   little-endian length (0xFFFFFFFF for NULL) and the bytes. */
void put_value(std::string *out, const char *p, size_t n);
void put_null(std::string *out);

}  // namespace vdb_proto

#endif
