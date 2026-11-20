/* images.cc: the image callback of D-86. When a foreign-key cascade writes
   child rows that mysqld never sees, vdb-mysql asks for their sort images:
   the child table is opened from its share into a private TABLE (never one
   the executor is using), each row is stored with Field::store and its
   images are built with MySQL's own Field::make_sort_key. */
#include "m_string.h"
#include "sql/field.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/table.h"
#include "storage/vdb/storage/vdb_util.h"

void vdb_compute_images(THD *thd, const vdb::ImagesNeeded &q, vdb::ImagesReply *r) {
  const std::string &db = q.database(), &name = q.name();
  /* The table definition cache key: "db\0table\0", as sql_base.cc builds it. */
  char key[NAME_LEN * 2 + 2];
  size_t key_length = strmake(strmake(key, db.c_str(), NAME_LEN) + 1, name.c_str(), NAME_LEN) - key + 1;
  mysql_mutex_lock(&LOCK_open);
  TABLE_SHARE *share = get_table_share(thd, db.c_str(), name.c_str(), key, key_length, false);
  mysql_mutex_unlock(&LOCK_open);
  if (share == nullptr) {
    r->mutable_error()->set_message("VDB could not open " + db + "." + name + " for a cascade");
    return;
  }
  TABLE tbl;
  if (open_table_from_share(thd, share, name.c_str(), 0, EXTRA_RECORD, 0, &tbl, false, nullptr)) {
    mysql_mutex_lock(&LOCK_open);
    release_table_share(share);
    mysql_mutex_unlock(&LOCK_open);
    r->mutable_error()->set_message("VDB could not open " + db + "." + name + " for a cascade");
    return;
  }
  my_bitmap_map *old = dbug_tmp_use_all_columns(&tbl, tbl.write_set);
  for (const vdb::Row &row : q.rows()) {
    memcpy(tbl.record[0], share->default_values, share->reclength);
    for (int i = 0; i < row.values_size() && i < static_cast<int>(share->fields); i++)
      vdb_store_value(tbl.field[i], row.values(i));
    vdb_images_of(&tbl, r->add_rows());
  }
  dbug_tmp_restore_column_map(tbl.write_set, old);
  closefrm(&tbl, false);
  mysql_mutex_lock(&LOCK_open);
  release_table_share(share);
  mysql_mutex_unlock(&LOCK_open);
}
