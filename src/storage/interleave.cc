/* interleave.cc: the one merge step in C++ (product doc 03 §2.4, D-48). An
   ordered scan has two cursors from vdb-mysql: Source rows in index order
   with touched keys removed, and Delta rows in image order. This puts them in
   order by the index's sort image (with the identity key as tie-break); on
   equal keys the Delta row wins and the Source row is dropped (D-33). Each
   cursor is checked to be monotonic; a violation fails the statement and
   counts vdb_order_violations. Each cursor's next row is decoded once, into
   its own head buffer, and the row taken is copied from there. Only indexes
   whose collation is proven for order are read in order (D-59), so image
   order is MySQL's order here. */
#include "sql/handler.h"
#include "sql/table.h"
#include "storage/vdb/common/errors.h"
#include "storage/vdb/storage/ha_vdb.h"

int ha_vdb_mysql_storage::open_ordered(const vdb::Range *range, bool back) {
  scan_close();
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
  pick_columns(false);
  add_key_columns(active_index);
  vdb::ScanOpen m;
  m.set_table(tid);
  m.set_version(tversion);
  m.set_index(active_index);
  m.set_kind(active_index == identity ? vdb::SCAN_PRIMARY_RANGE : vdb::SCAN_INDEX_RANGE);
  if (range) *m.add_ranges() = *range;
  m.set_backward(back);
  for (uint32_t col : cols) m.add_columns(col);
  m.set_pushed(pushed);
  m.set_lock(static_cast<vdb::LockMode>(lock_mode));
  vdb::ScanOpenReply r;
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_SCAN_OPEN, m, &r, &e);
  vdb_bump(vdb_st_requests);
  if (rc != 0) return fail(rc, e);
  scan_id = r.scan();
  scan_open = true;
  ordered = true;
  backward = back;
  src.batch = r.source();
  src.pos = 0;
  src.end = r.source().end();
  dlt.batch = r.delta();
  dlt.pos = 0;
  dlt.end = r.delta().end();
  return 0;
}

namespace {
/* The comparison image of the row in rec: the index's parts then the identity's. */
std::string head_image(ha_vdb_mysql_storage *h, TABLE *t, uint idx, uint identity, const uchar *rec) {
  std::string img = h->key_image(idx, rec, t->key_info[idx].user_defined_key_parts);
  if (identity != MAX_KEY && identity != idx)
    img += h->key_image(identity, rec, t->key_info[identity].user_defined_key_parts);
  return img;
}
}  // namespace

int ha_vdb_mysql_storage::next_ordered(uchar *buf) {
  for (;;) {
    if (!have_src_head) {
      if (src.more()) {
        if (int e = decode_row(src.batch.rows(src.pos), src_head)) return e;
        src_img = head_image(this, table, active_index, identity, src_head);
        have_src_head = true;
      } else if (!src.end) {
        if (int e = fetch_more(&src, vdb::CURSOR_SOURCE)) return e;
        continue;
      }
    }
    if (!have_dlt_head) {
      if (dlt.more()) {
        if (int e = decode_row(dlt.batch.rows(dlt.pos), dlt_head)) return e;
        dlt_img = head_image(this, table, active_index, identity, dlt_head);
        have_dlt_head = true;
      } else if (!dlt.end) {
        if (int e = fetch_more(&dlt, vdb::CURSOR_DELTA)) return e;
        continue;
      }
    }
    if (!have_src_head && !have_dlt_head) return HA_ERR_END_OF_FILE;
    bool take_src;
    if (have_src_head && have_dlt_head) {
      int cmp = src_img.compare(dlt_img);
      if (backward) cmp = -cmp;
      if (cmp == 0 && identity != MAX_KEY) {
        /* Equal keys: the Delta row wins; the Source row is hidden. */
        src.pos++;
        have_src_head = false;
        continue;
      }
      take_src = cmp <= 0;
    } else {
      take_src = have_src_head;
    }
    Vdb_rows &rows = take_src ? src : dlt;
    std::string &img = take_src ? src_img : dlt_img;
    std::string &prev = take_src ? prev_src : prev_dlt;
    if (!prev.empty()) {
      int c = prev.compare(img);
      if (backward ? c < 0 : c > 0) {
        vdb_bump(vdb_st_order_violations);
        err_msg = std::string("VDB: rows of index ") + table->key_info[active_index].name +
                  " arrived out of order; its collation is not proven for order";
        return HA_ERR_VDB;
      }
    }
    prev = img;
    int i = rows.pos++;
    (take_src ? have_src_head : have_dlt_head) = false;
    copy_row(take_src ? src_head : dlt_head, buf);
    remember(rows.batch.refs(i));
    return 0;
  }
}
