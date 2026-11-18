/* proto.cc: frame reading and writing over a Unix socket (doc 07 §1). */
#include "storage/vdb/common/proto.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace vdb_proto {

static bool write_full(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t k = ::write(fd, p, n);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) return false;
    p += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

static bool read_full(int fd, char *p, size_t n) {
  while (n > 0) {
    ssize_t k = ::read(fd, p, n);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) return false;
    p += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

bool write_frame(int fd, uint8_t op, const std::string &body) {
  if (body.size() > kMaxBody) return false;
  char hdr[5];
  hdr[0] = static_cast<char>(op);
  uint32_t n = static_cast<uint32_t>(body.size());
  for (int i = 0; i < 4; i++) hdr[1 + i] = static_cast<char>((n >> (8 * i)) & 0xff);
  return write_full(fd, hdr, 5) && write_full(fd, body.data(), body.size());
}

bool read_frame(int fd, uint8_t *op, std::string *body) {
  unsigned char hdr[5];
  if (!read_full(fd, reinterpret_cast<char *>(hdr), 5)) return false;
  uint32_t n = static_cast<uint32_t>(hdr[1]) | (static_cast<uint32_t>(hdr[2]) << 8) |
               (static_cast<uint32_t>(hdr[3]) << 16) | (static_cast<uint32_t>(hdr[4]) << 24);
  if (n > kMaxBody) return false;
  *op = hdr[0];
  body->resize(n);
  return n == 0 || read_full(fd, body->data(), n);
}

void put_value(std::string *out, const char *p, size_t n) {
  uint32_t len = static_cast<uint32_t>(n);
  char b[4];
  for (int i = 0; i < 4; i++) b[i] = static_cast<char>((len >> (8 * i)) & 0xff);
  out->append(b, 4);
  out->append(p, n);
}

void put_null(std::string *out) { out->append("\xff\xff\xff\xff", 4); }

}  // namespace vdb_proto
