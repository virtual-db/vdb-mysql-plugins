/* rows.cc: value conversion (product doc 04 §3). Values travel in MySQL's
   text form and are converted with MySQL's own Field::val_str and
   Field::store; TIMESTAMP values travel as Unix time with fractions, so the
   session's time zone never makes them ambiguous. */
#include <cstdlib>
#include <cstring>

#include "my_time_t.h"
#include "sql/field.h"
#include "sql/table.h"
#include "sql_string.h"
#include "storage/vdb/storage/ha_vdb.h"

namespace {

/* "1700000000.123456" to my_timeval. */
my_timeval parse_unix(const std::string &s) {
  my_timeval tv{0, 0};
  const char *p = s.c_str();
  char *end = nullptr;
  tv.m_tv_sec = strtoll(p, &end, 10);
  if (end && *end == '.') {
    int64_t us = 0;
    int digits = 0;
    for (const char *q = end + 1; *q >= '0' && *q <= '9' && digits < 6; q++, digits++) us = us * 10 + (*q - '0');
    for (; digits < 6; digits++) us *= 10;
    tv.m_tv_usec = us;
  }
  return tv;
}

}  // namespace

/* Store one batch row into buf: the values are in the order of cols. */
int ha_vdb_mysql_storage::decode_row(const vdb::Row &row, uchar *buf) {
  memcpy(buf, table->s->default_values, table->s->null_bytes);
  ptrdiff_t off = buf - table->record[0];
  my_bitmap_map *old = dbug_tmp_use_all_columns(table, table->write_set);
  for (int i = 0; i < row.values_size() && i < static_cast<int>(cols.size()); i++) {
    Field *f = table->field[cols[i]];
    const vdb::Value &v = row.values(i);
    f->move_field_offset(off);
    if (v.null()) {
      f->set_null();
    } else {
      f->set_notnull();
      if (f->type() == MYSQL_TYPE_TIMESTAMP) {
        my_timeval tv = parse_unix(v.b());
        f->store_timestamp(&tv);
      } else {
        f->store(v.b().data(), v.b().size(), f->charset());
      }
    }
    f->move_field_offset(-off);
  }
  dbug_tmp_restore_column_map(table->write_set, old);
  vdb_bump(vdb_st_rows_received);
  return 0;
}

/* Encode fields of a record as text values. */
void ha_vdb_mysql_storage::encode_fields(const uchar *rec, const std::vector<Field *> &fs, vdb::Row *out) {
  ptrdiff_t off = rec - table->record[0];
  my_bitmap_map *old = dbug_tmp_use_all_columns(table, table->read_set);
  char tmp[MAX_FIELD_WIDTH];
  for (Field *f : fs) {
    vdb::Value *v = out->add_values();
    f->move_field_offset(off);
    if (f->is_null()) {
      v->set_null(true);
    } else if (f->type() == MYSQL_TYPE_TIMESTAMP) {
      my_timeval tv;
      int warnings = 0;
      f->get_timestamp(&tv, &warnings);
      char b[48];
      int n = snprintf(b, sizeof(b), "%lld.%06lld", static_cast<long long>(tv.m_tv_sec),
                       static_cast<long long>(tv.m_tv_usec));
      v->set_b(b, n);
    } else {
      String s(tmp, sizeof(tmp), f->charset());
      String *r = f->val_str(&s);
      v->set_b(r->ptr(), r->length());
    }
    f->move_field_offset(-off);
  }
  dbug_tmp_restore_column_map(table->read_set, old);
}

/* Every column of a record, in definition order (the Delta holds whole rows). */
void ha_vdb_mysql_storage::encode_row(const uchar *rec, vdb::Row *out) {
  std::vector<Field *> all(table->field, table->field + table->s->fields);
  encode_fields(rec, all, out);
  vdb_bump(vdb_st_rows_sent);
}

/* Free forms for tables other than the handler's own (ALTER conversion,
   cascade images): they work on the fields where they point. */
void vdb_store_value(Field *f, const vdb::Value &v) {
  if (v.null()) {
    f->set_null();
    return;
  }
  f->set_notnull();
  if (f->type() == MYSQL_TYPE_TIMESTAMP) {
    my_timeval tv = parse_unix(v.b());
    f->store_timestamp(&tv);
  } else {
    f->store(v.b().data(), v.b().size(), f->charset());
  }
}

void vdb_value_of(Field *f, vdb::Value *v) {
  if (f->is_null()) {
    v->set_null(true);
    return;
  }
  if (f->type() == MYSQL_TYPE_TIMESTAMP) {
    my_timeval tv;
    int warnings = 0;
    f->get_timestamp(&tv, &warnings);
    char b[48];
    int n = snprintf(b, sizeof(b), "%lld.%06lld", static_cast<long long>(tv.m_tv_sec),
                     static_cast<long long>(tv.m_tv_usec));
    v->set_b(b, n);
    return;
  }
  char tmp[MAX_FIELD_WIDTH];
  String s(tmp, sizeof(tmp), f->charset());
  String *r = f->val_str(&s);
  v->set_b(r->ptr(), r->length());
}
