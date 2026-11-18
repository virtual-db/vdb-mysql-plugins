/* cond.cc: pushed conditions (product doc 08 §7.1, §7.2). Each AND part on
   this table alone is printed with MySQL's own printer; every cached constant
   (<cache>(expr)) is replaced by the value mysqld computed, as a literal with
   its character set introducer and collation, so the Source never evaluates
   it in its own session. The whole condition is returned as the remainder:
   mysqld re-checks every row, the pushed text only narrows the Source read.
   Which parts reach the Source is decided in Go (vdb-mysql/internal/pushdown). */
#include <cstdio>
#include <string>
#include <vector>

#include "sql-common/my_decimal.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/table.h"
#include "storage/vdb/storage/ha_vdb.h"

namespace {

std::string hex(const char *p, size_t n) {
  static const char d[] = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    out.push_back(d[(static_cast<unsigned char>(p[i]) >> 4) & 15]);
    out.push_back(d[static_cast<unsigned char>(p[i]) & 15]);
  }
  return out;
}

/* The literal of a cached value, or false when it cannot be rendered. */
bool cache_literal(Item_cache *c, std::string *lit) {
  switch (c->result_type()) {
    case INT_RESULT: {
      longlong x = c->val_int();
      *lit = c->null_value ? "NULL" : (c->unsigned_flag ? std::to_string(static_cast<ulonglong>(x)) : std::to_string(x));
      return true;
    }
    case DECIMAL_RESULT: {
      my_decimal d;
      my_decimal *dp = c->val_decimal(&d);
      if (c->null_value || dp == nullptr) {
        *lit = "NULL";
        return true;
      }
      String t;
      my_decimal2string(E_DEC_FATAL_ERROR, dp, &t);
      lit->assign(t.ptr(), t.length());
      return true;
    }
    case REAL_RESULT:
      return false;  // FLOAT and DOUBLE are never pushed
    default: {
      char buf[MAX_FIELD_WIDTH];
      String v(buf, sizeof(buf), &my_charset_bin);
      String *sv = c->val_str(&v);
      if (c->null_value || sv == nullptr) {
        *lit = "NULL";
        return true;
      }
      const CHARSET_INFO *cs = sv->charset();
      *lit = std::string("_") + cs->csname + " X'" + hex(sv->ptr(), sv->length()) + "'";
      if (cs != &my_charset_bin) *lit += std::string(" COLLATE ") + c->collation.collation->m_coll_name;
      return true;
    }
  }
}

void collect(THD *thd, const Item *cond, table_map self, std::string *out) {
  if (cond->type() == Item::COND_ITEM &&
      down_cast<const Item_cond *>(cond)->functype() == Item_func::COND_AND_FUNC) {
    for (const Item &i : *const_cast<Item_cond *>(down_cast<const Item_cond *>(cond))->argument_list())
      collect(thd, &i, self, out);
    return;
  }
  const table_map used = cond->used_tables();
  if ((used & ~self) != 0 || cond->has_subquery() || cond->has_stored_program()) return;
  String s;
  cond->print(thd, &s, enum_query_type(QT_ORDINARY | QT_NO_DB));
  std::string text(s.ptr(), s.length());
  std::vector<Item_cache *> caches;
  WalkItem(const_cast<Item *>(cond), enum_walk::PREFIX, [&](Item *i) {
    if (i->type() == Item::CACHE_ITEM) caches.push_back(down_cast<Item_cache *>(i));
    return false;
  });
  static const std::string tag = "<cache>(";
  size_t from = 0, ci = 0;
  while ((from = text.find(tag, from)) != std::string::npos) {
    if (ci >= caches.size()) return;  // cannot map: do not push this part
    size_t i = from + tag.size(), depth = 1;
    for (; i < text.size() && depth; i++) {
      if (text[i] == '\'') {
        for (i++; i < text.size() && text[i] != '\''; i++)
          if (text[i] == '\\') i++;
      } else if (text[i] == '(') {
        depth++;
      } else if (text[i] == ')') {
        depth--;
      }
    }
    std::string inner = text.substr(from, i - from);
    size_t nested = 0;
    for (size_t k = inner.find(tag, 1); k != std::string::npos; k = inner.find(tag, k + 1)) nested++;
    std::string lit;
    if (!cache_literal(caches[ci], &lit)) return;
    ci += 1 + nested;
    text.replace(from, i - from, lit);
    from += lit.size();
  }
  if (!out->empty()) out->append(" and ");
  out->append("(" + text + ")");
}

}  // namespace

void ha_vdb_mysql_storage::push_scan_condition(THD *thd, const Item *cond) {
  if (!table->pos_in_table_list) return;
  std::string s;
  collect(thd, cond, table->pos_in_table_list->map(), &s);
  pushed = s;
}

/* UPDATE and DELETE push here; the whole condition stays the remainder. */
const Item *ha_vdb_mysql_storage::cond_push(const Item *cond) {
  push_scan_condition(ha_thd(), cond);
  return cond;
}

/* Index conditions are not evaluated in the engine. */
Item *ha_vdb_mysql_storage::idx_cond_push(uint, Item *idx_cond) { return idx_cond; }

/* SELECT: mysqld hands the finished plan to the engine; push the filter that
   sits directly on each VDB table access. */
int vdb_push_to_engine(THD *thd, AccessPath *root, JOIN *join) {
  if (!thd->optimizer_switch_flag(OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN)) return 0;
  WalkAccessPaths(root, join, WalkAccessPathPolicy::ENTIRE_QUERY_BLOCK, [thd](AccessPath *p, const JOIN *) {
    if (p->type != AccessPath::FILTER) return false;
    AccessPath *child = p->filter().child;
    TABLE *t = nullptr;
    switch (child->type) {
      case AccessPath::TABLE_SCAN:
        t = child->table_scan().table;
        break;
      case AccessPath::INDEX_SCAN:
        t = child->index_scan().table;
        break;
      case AccessPath::INDEX_RANGE_SCAN:
        t = child->index_range_scan().used_key_part[0].field->table;
        break;
      default:
        break;
    }
    if (t != nullptr)
      if (auto *h = dynamic_cast<ha_vdb_mysql_storage *>(t->file)) h->push_scan_condition(thd, p->filter().condition);
    return false;
  });
  return 0;
}
