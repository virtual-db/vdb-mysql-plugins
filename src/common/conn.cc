/* conn.cc: socket, framing, Hello, and the Waiting and ImagesNeeded frames
   (implementation plan doc 07 §1, D-86). */
#include "storage/vdb/common/conn.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

char *vdb_socket_path = nullptr;
const char *vdb_build = "vdb.so 0.1.0 for mysqld " MYSQL_SERVER_VERSION;

Vdb_conn::~Vdb_conn() { close(); }

void Vdb_conn::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool Vdb_conn::connect(vdb::ConnKind kind, uint64_t conn_id, std::string *err) {
  close();
  const char *path = vdb_socket_path ? vdb_socket_path : "/run/vdb/vdb.sock";
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *err = "VDB is unavailable: cannot create a socket";
    return false;
  }
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
    ::close(fd);
    *err = "VDB is unavailable";
    return false;
  }
  fd_ = fd;
  vdb::Hello hello;
  hello.set_version(vdb_proto::kVersion);
  hello.set_kind(kind);
  hello.set_build(vdb_build);
  hello.set_conn_id(conn_id);
  vdb::HelloReply reply;
  vdb::Error e;
  int rc = call(vdb_proto::OP_HELLO, hello, &reply, &e);
  if (rc != 0) {
    *err = rc == 1 ? "VDB is unavailable: " + e.message() : "VDB is unavailable";
    close();
    return false;
  }
  query_intercept_ = reply.query_intercept();
  return true;
}

bool Vdb_conn::send(uint8_t op, const google::protobuf::MessageLite &req) {
  if (fd_ < 0) return false;
  std::string body;
  if (!req.SerializeToString(&body) || !vdb_proto::write_frame(fd_, op, body)) {
    close();
    return false;
  }
  return true;
}

int Vdb_conn::call(uint8_t op, const google::protobuf::MessageLite &req, google::protobuf::MessageLite *reply,
                   vdb::Error *err) {
  if (!send(op, req)) return -1;
  for (;;) {
    uint8_t rop;
    std::string body;
    if (!vdb_proto::read_frame(fd_, &rop, &body)) {
      close();
      return -1;
    }
    if (rop == vdb_proto::OP_WAITING) {
      vdb::Waiting w;
      if (w.ParseFromString(body) && on_wait) on_wait(w.holder());
      continue;
    }
    if (rop == vdb_proto::OP_IMAGES_NEEDED) {
      vdb::ImagesNeeded q;
      vdb::ImagesReply r;
      if (!q.ParseFromString(body)) {
        close();
        return -1;
      }
      if (on_images)
        on_images(q, &r);
      else
        r.mutable_error()->set_message("no image callback in this call");
      if (!send(vdb_proto::OP_IMAGES_NEEDED, r)) return -1;
      continue;
    }
    if (rop == vdb_proto::OP_ERROR) {
      if (err && !err->ParseFromString(body)) err->set_message("malformed error reply");
      if (err && err->code() >= vdb_proto::E_PROTOCOL) close();
      return 1;
    }
    if (rop != static_cast<uint8_t>(op | vdb_proto::kReplyBit)) {
      close();
      return -1;
    }
    if (reply && !reply->ParseFromString(body)) {
      close();
      return -1;
    }
    return 0;
  }
}
