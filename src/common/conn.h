/* conn.h: one connection to vdb-mysql over the plug-in socket (doc 07 §1):
   connect and Hello, send a request and read its reply, handle the Waiting
   notice and ImagesNeeded callbacks that may come before the final reply. */
#ifndef VDB_COMMON_CONN_H
#define VDB_COMMON_CONN_H

#include <cstdint>
#include <functional>
#include <string>

#include "storage/vdb/common/proto.h"

/* The socket path, from the system variable vdb_mysql_storage_socket. */
extern char *vdb_socket_path;
/* The library's build string, sent in Hello. */
extern const char *vdb_build;

class Vdb_conn {
 public:
  ~Vdb_conn();

  /* Connect and send Hello with the connection kind and the mysqld
     connection id. On failure *err says why. */
  bool connect(vdb::ConnKind kind, uint64_t conn_id, std::string *err);

  /* Send a request and read its reply. Returns 0 on success, 1 when
     vdb-mysql replied with an error (in *err), -1 when the connection failed
     (it is then closed; the next statement reconnects). */
  int call(uint8_t op, const google::protobuf::MessageLite &req, google::protobuf::MessageLite *reply, vdb::Error *err);

  /* Send a frame with no reply (Event). Returns false on failure. */
  bool send(uint8_t op, const google::protobuf::MessageLite &req);

  /* Called with the holder's connection id when a Waiting frame arrives. */
  std::function<void(uint64_t)> on_wait;
  /* Called for an ImagesNeeded frame; fills the reply. */
  std::function<void(const vdb::ImagesNeeded &, vdb::ImagesReply *)> on_images;

  bool is_open() const { return fd_ >= 0; }
  bool query_intercept() const { return query_intercept_; }
  void close();

 private:
  int fd_ = -1;
  bool query_intercept_ = false;
};

#endif
