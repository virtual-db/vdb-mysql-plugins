/* write.cc: writes (product doc 10 §2). write_row sends the whole row with
   the sort image of every index; duplicates come back with the index that
   collided, so mysqld sees each at the row where it occurs. Bulk inserts are
   buffered and sent in batches when the statement has no IGNORE, REPLACE or
   ON DUPLICATE KEY handling. update_row and delete_row send back the
   reference of the row the read returned, with its whole old row. */
#include "sql/handler.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/table.h"
#include "storage/vdb/common/errors.h"
#include "storage/vdb/storage/ha_vdb.h"

namespace {
constexpr int kInsertBatch = 1000;
}

void ha_vdb_mysql_storage::start_bulk_insert(ha_rows) {
  THD *thd = ha_thd();
  LEX *lex = thd->lex;
  bulk = lex != nullptr && lex->duplicates == DUP_ERROR && !lex->is_ignore();
  bulk_req.Clear();
}

int ha_vdb_mysql_storage::flush_bulk() {
  if (bulk_req.rows_size() == 0) return 0;
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
  bulk_req.set_table(tid);
  vdb::InsertReply r;
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_INSERT_BATCH, bulk_req, &r, &e);
  vdb_bump(vdb_st_requests);
  bulk_req.Clear();
  if (rc != 0) return fail(rc, e);
  for (const auto &res : r.results())
    if (!res.ok()) return fail(1, res.error());
  return 0;
}

int ha_vdb_mysql_storage::end_bulk_insert() {
  int e = flush_bulk();
  bulk = false;
  return e;
}

int ha_vdb_mysql_storage::write_row(uchar *buf) {
  if (table->next_number_field && buf == table->record[0]) {
    if (int e = update_auto_increment()) return e;
  }
  if (bulk) {
    encode_row(buf, bulk_req.add_rows());
    row_images(buf, bulk_req.add_images());
    if (bulk_req.rows_size() >= kInsertBatch) return flush_bulk();
    return 0;
  }
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
  vdb::Insert m;
  m.set_table(tid);
  encode_row(buf, m.mutable_row());
  row_images(buf, m.mutable_images());
  vdb::InsertReply r;
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_INSERT, m, &r, &e);
  vdb_bump(vdb_st_requests);
  if (rc != 0) return fail(rc, e);
  if (r.results_size() > 0 && !r.results(0).ok()) return fail(1, r.results(0).error());
  return 0;
}

int ha_vdb_mysql_storage::update_row(const uchar *old_data, uchar *new_data) {
  if (bulk) flush_bulk();
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
  vdb::Update m;
  m.set_table(tid);
  *m.mutable_old() = cur_ref;
  m.mutable_old()->clear_whole();
  encode_row(old_data, m.mutable_old()->mutable_whole());
  encode_row(new_data, m.mutable_row());
  row_images(new_data, m.mutable_images());
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_UPDATE, m, nullptr, &e);
  vdb_bump(vdb_st_requests);
  if (rc != 0) return fail(rc, e);
  return 0;
}

int ha_vdb_mysql_storage::delete_row(const uchar *buf) {
  if (bulk) flush_bulk();
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
  vdb::Delete m;
  m.set_table(tid);
  *m.mutable_old() = cur_ref;
  m.mutable_old()->clear_whole();
  encode_row(buf, m.mutable_old()->mutable_whole());
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_DELETE, m, nullptr, &e);
  vdb_bump(vdb_st_requests);
  if (rc != 0) return fail(rc, e);
  return 0;
}

/* A block of auto-increment values from vdb-core (D-15). */
void ha_vdb_mysql_storage::get_auto_increment(ulonglong, ulonglong, ulonglong nb_desired, ulonglong *first,
                                              ulonglong *nb_reserved) {
  *first = ~static_cast<ulonglong>(0);
  *nb_reserved = 0;
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return;
  vdb::AutoIncrement m;
  m.set_table(tid);
  m.set_count(nb_desired == 0 ? 1 : nb_desired);
  vdb::AutoIncrementReply r;
  vdb::Error e;
  if (c->call(vdb_proto::OP_AUTO_INCREMENT, m, &r, &e) != 0) return;
  *first = r.first();
  *nb_reserved = r.granted();
}
