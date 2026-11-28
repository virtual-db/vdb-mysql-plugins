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
        if (f->is_flag_set(BLOB_FLAG) && buf != table->record[0]) {
          f->move_field_offset(-off);
          detach_blob(down_cast<Field_blob *>(f), cols[i]);
          f->move_field_offset(off);
        }
        f->store(v.b().data(), v.b().size(), f->charset());
        if (f->is_flag_set(BLOB_FLAG)) keep_blob(down_cast<Field_blob *>(f), buf, cols[i]);
      }
    }
    f->move_field_offset(-off);
  }
  dbug_tmp_restore_column_map(table->write_set, old);
  vdb_bump(vdb_st_rows_received);
  return 0;
}

/* A BLOB, TEXT or JSON field stores its value in a buffer the Field object
   owns, and the record only points to it, so every record decoded through
   the same Field would point to the last value stored. Copy the value into
   storage kept per record buffer and field, and point the record there, so
   record[0] and record[1] (and any other buffer) keep their own values.
   The copy lives until the next row is decoded into the same buffer. */
/* Before a value is decoded into another buffer through the same Field,
   give record[0]'s value its own copy: mysqld may have stored the row it is
   writing there (REPLACE reads the conflicting row into record[1] while
   record[0] holds the new row), and that value lives in the Field's buffer
   the store is about to overwrite. The Field points at record[0] here. */
void ha_vdb_mysql_storage::detach_blob(Field_blob *b, uint32_t field) {
  const uint32 len = b->get_length();
  if (len == 0) return;
  std::string copy(reinterpret_cast<const char *>(b->get_blob_data()), len);
  std::vector<std::string> &slots = blob_store[table->record[0]];
  if (slots.size() <= field) slots.resize(table->s->fields);
  slots[field] = std::move(copy);
  b->set_ptr(len, reinterpret_cast<const uchar *>(slots[field].data()));
}

void ha_vdb_mysql_storage::keep_blob(Field_blob *b, const uchar *buf, uint32_t field) {
  std::vector<std::string> &slots = blob_store[buf];
  if (slots.size() <= field) slots.resize(table->s->fields);
  const uint32 len = b->get_length();
  slots[field].assign(reinterpret_cast<const char *>(b->get_blob_data()), len);
  b->set_ptr(len, reinterpret_cast<const uchar *>(slots[field].data()));
}

/* Copy a row decode_row stored in another buffer into buf: the null bytes
   and the requested columns, the same bytes decode_row would have written.
   A NULL column's bytes are left as they are, as decode_row leaves them:
   the head buffer's bytes there were never written (a BLOB's length and
   pointer there are garbage that detach_blob would later read). A BLOB,
   TEXT or JSON value gets buf's own copy, so it lives as long as one
   decoded into buf directly (keep_blob). */
void ha_vdb_mysql_storage::copy_row(const uchar *from, uchar *buf) {
  memcpy(buf, from, table->s->null_bytes);
  ptrdiff_t off = buf - table->record[0];
  for (uint32_t c : cols) {
    Field *f = table->field[c];
    if (f->is_real_null(off)) continue;
    const size_t at = f->offset(table->record[0]);
    memcpy(buf + at, from + at, f->pack_length());
    if (f->is_flag_set(BLOB_FLAG)) {
      f->move_field_offset(off);
      keep_blob(down_cast<Field_blob *>(f), buf, c);
      f->move_field_offset(-off);
    }
  }
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
