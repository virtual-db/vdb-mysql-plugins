/* preparse.cc: MYSQL_AUDIT_PARSE_PREPARSE (doc 08 §5). For a statement whose
   first keyword is CREATE, ask vdb-mysql which ENGINE clauses to remove
   (D-54) and splice them out: the decision is Go's, C++ only removes the
   byte ranges. When an intercept handler is attached in vdb-mysql, every
   statement is sent as QueryReceived instead and the returned text applied. */
#include <cctype>
#include <cstring>
#include <string>

#include "my_sys.h"
#include "sql/sql_class.h"
#include "storage/vdb/statements/statements.h"

static PSI_memory_key key_memory_vdb_rewrite = PSI_NOT_INSTRUMENTED;

namespace {

/* The first keyword, after whitespace and comments. */
std::string first_keyword(const char *s, size_t n) {
  size_t i = 0;
  for (;;) {
    while (i < n && isspace(static_cast<unsigned char>(s[i]))) i++;
    if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
      const char *end = static_cast<const char *>(memmem(s + i + 2, n - i - 2, "*/", 2));
      if (end == nullptr) return "";
      i = static_cast<size_t>(end - s) + 2;
      continue;
    }
    if ((i + 1 < n && s[i] == '-' && s[i + 1] == '-') || (i < n && s[i] == '#')) {
      while (i < n && s[i] != '\n') i++;
      continue;
    }
    break;
  }
  std::string w;
  while (i < n && isalpha(static_cast<unsigned char>(s[i]))) w.push_back(static_cast<char>(toupper(s[i++])));
  return w;
}

void replace_query(const mysql_event_parse *ev, const std::string &text) {
  char *q = static_cast<char *>(my_malloc(key_memory_vdb_rewrite, text.size() + 1, MYF(0)));
  if (q == nullptr) return;
  memcpy(q, text.data(), text.size());
  q[text.size()] = 0;
  ev->rewritten_query->str = q;
  ev->rewritten_query->length = text.size();
  *reinterpret_cast<int *>(ev->flags) |= static_cast<int>(MYSQL_AUDIT_PARSE_REWRITE_PLUGIN_QUERY_REWRITTEN);
  __atomic_add_fetch(&vdb_st_rewrites, 1, __ATOMIC_RELAXED);
}

}  // namespace

int vdb_on_preparse(MYSQL_THD thd, const mysql_event_parse *ev) {
  if (vdb_statements_skip(thd)) return 0;
  std::string query(ev->query.str, ev->query.length);
  bool intercept = false;
  if (first_keyword(query.data(), query.size()) == "CREATE") {
    vdb::Rewrite m;
    m.set_conn_id(thd_get_thread_id(thd));
    m.set_statement(query);
    vdb::RewriteReply r;
    vdb::Error e;
    if (vdb_event_call(vdb_proto::OP_REWRITE, m, &r, &e, &intercept) == 0 && !r.unchanged()) {
      std::string out;
      size_t last = 0;
      for (const auto &rg : r.remove()) {
        if (rg.start() < last || rg.end() > query.size()) continue;
        out.append(query, last, rg.start() - last);
        last = rg.end();
      }
      out.append(query, last, std::string::npos);
      query = out;
      replace_query(ev, query);
    }
  } else {
    vdb::Empty none;  // op 0: only learn whether intercept is on
    vdb_event_call(0, none, nullptr, nullptr, &intercept);
  }
  if (intercept) {
    vdb::QueryReceived m;
    m.set_conn_id(thd_get_thread_id(thd));
    m.set_statement(query);
    if (thd->db().str) m.set_database(thd->db().str, thd->db().length);
    vdb::QueryReceivedReply r;
    vdb::Error e;
    int rc = vdb_event_call(vdb_proto::OP_QUERY_RECEIVED, m, &r, &e, nullptr);
    if (rc == 1) {
      my_printf_error(ER_UNKNOWN_ERROR, "VDB: %s", MYF(0), e.message().c_str());
      return 1;
    }
    if (rc == 0 && r.statement() != query) replace_query(ev, r.statement());
  }
  return 0;
}
