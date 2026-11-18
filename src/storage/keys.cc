/* keys.cc: keys and sort images (product doc 06, D-38). An image is built
   with MySQL's own Field::make_sort_key, padded as filesort pads it, with a
   NULL marker byte for nullable parts; every part has a fixed width, so the
   image of a key prefix is a prefix of the image of the full key. The raw
   form sent to vdb-mysql for Source SQL is each part's text value. */
#include <algorithm>
#include <cstring>

#include "m_ctype.h"
#include "my_time_t.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/key.h"
#include "sql/table.h"
#include "storage/vdb/common/proto.h"
#include "storage/vdb/storage/ha_vdb.h"

namespace {

/* The width of one key part's image, without the NULL marker. */
size_t part_width(const Field *f) {
  if (f->result_type() == STRING_RESULT && f->real_type() != MYSQL_TYPE_ENUM && f->real_type() != MYSQL_TYPE_SET &&
      !is_temporal_type(f->type())) {
    if (f->is_flag_set(BLOB_FLAG) || f->type() == MYSQL_TYPE_JSON) return 1024;
    const CHARSET_INFO *cs = f->charset();
    return cs->coll->strnxfrmlen(cs, f->max_display_length());
  }
  return f->pack_length();
}

void append_part(std::string *out, Field *f) {
  size_t w = part_width(f);
  if (f->is_nullable()) {
    if (f->is_null()) {
      out->push_back('\0');
      out->append(w, '\0');
      return;
    }
    out->push_back('\1');
  }
  size_t at = out->size();
  out->resize(at + w, '\0');
  f->make_sort_key(reinterpret_cast<uchar *>(out->data() + at), w);
}

}  // namespace

/* Width of each key part's image, for the catalog (part_lens). */
size_t vdb_part_len(const KEY_PART_INFO *kp) { return (kp->field->is_nullable() ? 1 : 0) + part_width(kp->field); }

std::string ha_vdb_mysql_storage::key_image(uint idx, const uchar *rec, uint parts) {
  std::string out;
  KEY *k = &table->key_info[idx];
  ptrdiff_t off = rec - table->record[0];
  my_bitmap_map *old = dbug_tmp_use_all_columns(table, table->read_set);
  for (uint i = 0; i < parts && i < k->user_defined_key_parts; i++) {
    Field *f = k->key_part[i].field;
    f->move_field_offset(off);
    append_part(&out, f);
    f->move_field_offset(-off);
  }
  dbug_tmp_restore_column_map(table->read_set, old);
  return out;
}

/* The image of every index of a row, by index number. */
void ha_vdb_mysql_storage::row_images(const uchar *rec, vdb::Images *out) {
  for (uint k = 0; k < table->s->keys; k++)
    out->add_images(key_image(k, rec, table->key_info[k].user_defined_key_parts));
}

/* The raw form of a key: each part's text value. */
std::string ha_vdb_mysql_storage::key_raw(uint idx, const uchar *rec, uint parts) {
  KEY *k = &table->key_info[idx];
  std::vector<Field *> fs;
  for (uint i = 0; i < parts && i < k->user_defined_key_parts; i++) fs.push_back(k->key_part[i].field);
  vdb::Row r;
  encode_fields(rec, fs, &r);
  std::string out;
  for (const auto &v : r.values()) {
    if (v.null())
      vdb_proto::put_null(&out);
    else
      vdb_proto::put_value(&out, v.b().data(), v.b().size());
  }
  return out;
}

/* How many key parts a key_part_map uses, and the key's length. */
uint ha_vdb_mysql_storage::parts_of(uint idx, key_part_map kpm, const uchar *, uint *key_len) {
  *key_len = calculate_key_len(table, idx, kpm);
  KEY *k = &table->key_info[idx];
  uint used = 0, n = 0;
  for (uint i = 0; i < k->user_defined_key_parts && used < *key_len; i++, n++) used += k->key_part[i].store_length;
  return n;
}

/* A key from MySQL's key buffer: restored into the scratch record, then its
   raw values and image. */
void ha_vdb_mysql_storage::make_range_key(uint idx, const uchar *key, uint key_len, uint parts, vdb::Key *out) {
  key_restore(scratch, key, &table->key_info[idx], key_len);
  out->set_raw(key_raw(idx, scratch, parts));
  out->set_image(key_image(idx, scratch, parts));
}

/* Whether a Source row is hidden by a collation-equal Delta key (doc 03 §2.4). */
bool ha_vdb_mysql_storage::shadowed(const uchar *rec) {
  if (shadows.empty() || identity == MAX_KEY) return false;
  std::string img = key_image(identity, rec, table->key_info[identity].user_defined_key_parts);
  return std::binary_search(shadows.begin(), shadows.end(), img);
}

/* The images of every index of the row in t->record[0]. */
void vdb_images_of(TABLE *t, vdb::Images *out) {
  my_bitmap_map *old = dbug_tmp_use_all_columns(t, t->read_set);
  for (uint k = 0; k < t->s->keys; k++) {
    std::string img;
    KEY *key = &t->key_info[k];
    for (uint i = 0; i < key->user_defined_key_parts; i++) append_part(&img, key->key_part[i].field);
    out->add_images(img);
  }
  dbug_tmp_restore_column_map(t->read_set, old);
}
