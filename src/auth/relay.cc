/* relay.cc: the exchange of product doc 03 §3.2. The Source decides every
   login: vdb-mysql opens a verified TLS connection to it and returns the
   Source's own challenge for the account; the plug-in sends it to the client
   as an authentication-method switch, forwards the client's answer, and on
   the Source's fast path the password never reaches VDB. When the Source
   wants the password, the plug-in asks the client exactly as a stock server
   does on a cache miss (in clear over TLS or a Unix socket, otherwise with
   mysqld's own RSA keys), sends it once and wipes it. The session then runs
   as the account vdb-mysql returns. Starts from the authentication spike,
   which passed 73 of 73 checks. */
#include "storage/vdb/auth/relay.h"

#include <cstring>
#include <string>

#include "crypt_genhash_impl.h"
#include "m_string.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/auth/auth_common.h"
#include "sql/auth/auth_internal.h"
#include "sql/auth/sql_authentication.h"
#include "sql/protocol_classic.h"
#include "storage/vdb/common/conn.h"
#include "violite.h"

extern Rsa_authentication_keys *g_caching_sha2_rsa_keys;

namespace {
const unsigned char kFastAuthSuccess = '\3';
const unsigned char kPerformFullAuth = '\4';
const unsigned char kRequestPublicKey = '\2';

void wipe(std::string *s) {
  if (!s->empty()) memset(s->data(), 0, s->size());
  s->clear();
}

/* A refusal from the Source keeps its own code; for "access denied" mysqld
   produces its standard message with the client's own host. */
int deny(uint32_t code, const std::string &msg) {
  if (code != 0 && code != ER_ACCESS_DENIED_ERROR) my_printf_error(code, "%s", MYF(0), msg.c_str());
  return CR_ERROR;
}

/* The login connection; its destructor aborts an unfinished login so the
   Source connection is never leaked. */
struct Login {
  Vdb_conn conn;
  bool open = false;
  ~Login() {
    if (open) {
      vdb::Empty m;
      conn.call(vdb_proto::OP_AUTH_ABORT, m, nullptr, nullptr);
    }
  }
};

int accept(const vdb::AuthReply &r, MYSQL_SERVER_AUTH_INFO *info, bool tls) {
  if (r.account().empty() || r.user().size() > USERNAME_CHAR_LENGTH) return CR_ERROR;
  /* REQUIRE SSL on the Source: the client must use TLS to reach VDB too; a
     Unix socket does not count, as in stock MySQL. */
  if (r.require_ssl() && !tls) return CR_ERROR;
  strmake(info->authenticated_as, r.user().c_str(), sizeof(info->authenticated_as) - 1);
  snprintf(info->external_user, sizeof(info->external_user), "%s", r.account().c_str());
  return CR_OK;
}
}  // namespace

int vdb_relay_authenticate(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info, const char *method) {
  MPVIO_EXT *mpvio = reinterpret_cast<MPVIO_EXT *>(vio);
  const enum_vio_type vt = mpvio->protocol->get_vio()->type;
  const bool secure = is_secure_transport(vt), tls = vt == VIO_TYPE_SSL;
  Login login;
  std::string err;
  if (!login.conn.connect(vdb::CONN_LOGIN, 0, &err)) return CR_ERROR;
  vdb::AuthBegin b;
  b.set_user(info->user_name, info->user_name_length);
  b.set_host(info->host_or_ip, info->host_or_ip_length);
  b.set_method(method);
  b.set_secure(secure);
  b.set_tls(tls);
  vdb::AuthBeginReply br;
  vdb::Error e;
  if (login.conn.call(vdb_proto::OP_AUTH_BEGIN, b, &br, &e) != 0) return CR_ERROR;
  if (br.refused()) return deny(br.code(), br.message());
  if (br.scramble().size() < SCRAMBLE_LENGTH) return CR_ERROR;
  login.open = true;

  /* The Source's challenge, sent to the client as this method's challenge. */
  char nonce[SCRAMBLE_LENGTH + 1];
  memcpy(nonce, br.scramble().data(), SCRAMBLE_LENGTH);
  nonce[SCRAMBLE_LENGTH] = 0;
  if (vio->write_packet(vio, reinterpret_cast<const unsigned char *>(nonce), SCRAMBLE_LENGTH + 1))
    return CR_AUTH_HANDSHAKE;
  memcpy(mpvio->scramble, nonce, SCRAMBLE_LENGTH + 1);

  unsigned char *pkt;
  int len = vio->read_packet(vio, &pkt);
  if (len < 0) return CR_AUTH_HANDSHAKE;
  const bool empty = len == 0 || (len == 1 && pkt[0] == 0);
  info->password_used = empty ? PASSWORD_USED_NO : PASSWORD_USED_YES;

  vdb::AuthAnswer a;
  a.set_answer(reinterpret_cast<const char *>(pkt), len);
  vdb::AuthReply ar;
  if (login.conn.call(vdb_proto::OP_AUTH_ANSWER, a, &ar, &e) != 0) return CR_ERROR;
  if (ar.result() == vdb::AUTH_ACCEPTED) {
    login.open = false;
    if (!empty && strcmp(method, "caching_sha2_password") == 0 && vio->write_packet(vio, &kFastAuthSuccess, 1))
      return CR_AUTH_HANDSHAKE;
    return accept(ar, info, tls);
  }
  if (ar.result() != vdb::AUTH_PASSWORD_NEEDED) {
    login.open = false;
    return deny(ar.code(), ar.message());
  }

  /* The Source wants the password: the same exchange as stock mysqld on a cache miss. */
  if (vio->write_packet(vio, &kPerformFullAuth, 1)) return CR_AUTH_HANDSHAKE;
  if ((len = vio->read_packet(vio, &pkt)) <= 0) return CR_AUTH_HANDSHAKE;
  std::string password;
  if (secure) {
    password.assign(reinterpret_cast<const char *>(pkt), strnlen(reinterpret_cast<const char *>(pkt), len));
    memset(pkt, 0, len);
  } else {
    Rsa_authentication_keys *keys = g_caching_sha2_rsa_keys;
    if (keys == nullptr || !keys->get_private_key()) return CR_ERROR;
    if (len == 1 && pkt[0] == kRequestPublicKey) {
      const char *pem = keys->get_public_key_as_pem();
      if (vio->write_packet(vio, reinterpret_cast<const unsigned char *>(pem), static_cast<int>(strlen(pem))))
        return CR_ERROR;
      if ((len = vio->read_packet(vio, &pkt)) <= 0) return CR_ERROR;
    }
    int cipher = keys->get_cipher_length();
    if (len != cipher || cipher > 1024) return CR_ERROR;
    unsigned char plain[1025];
    if (decrypt_RSA_private_key(pkt, cipher, plain, sizeof(plain) - 1, keys->get_private_key())) return CR_ERROR;
    plain[cipher] = 0;
    xor_string(reinterpret_cast<char *>(plain), cipher, nonce, SCRAMBLE_LENGTH);
    password.assign(reinterpret_cast<const char *>(plain), strnlen(reinterpret_cast<const char *>(plain), cipher));
    memset(plain, 0, sizeof(plain));
  }
  vdb::AuthPass p;
  p.set_password(password);
  wipe(&password);
  vdb::AuthReply pr;
  int rc = login.conn.call(vdb_proto::OP_AUTH_PASS, p, &pr, &e);
  std::string *pw = p.mutable_password();
  wipe(pw);
  login.open = false;  // vdb-mysql closes the Source connection after a password step
  if (rc != 0) return CR_ERROR;
  if (pr.result() != vdb::AUTH_ACCEPTED) return deny(pr.code(), pr.message());
  return accept(pr, info, tls);
}

/* No password is stored in VDB. */
int vdb_auth_generate(char *outbuf, unsigned int *outbuflen, const char *, unsigned int) {
  *outbuflen = 0;
  outbuf[0] = 0;
  return 0;
}
int vdb_auth_validate(char *const, unsigned int) { return 0; }
int vdb_auth_set_salt(const char *, unsigned int, unsigned char *, unsigned char *) { return 0; }
