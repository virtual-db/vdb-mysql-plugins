/* scan.cc: reads (product doc 03 §2.2): unordered scans in batches, the
   statement's positions, key lookups, ordered index scans in either
   direction, and range reads that send both bounds. Ordered scans interleave
   two cursors in interleave.cc. */
#include <algorithm>
#include <cstring>

#include "sql/handler.h"
#include "sql/key.h"
#include "sql/table.h"
#include "storage/vdb/common/errors.h"
#include "storage/vdb/storage/ha_vdb.h"

namespace {
void take_shadows(const google::protobuf::RepeatedPtrField<std::string> &in, std::vector<std::string> *out) {
  out->assign(in.begin(), in.end());
  std::sort(out->begin(), out->end());
}
}  // namespace

/* Make sure the columns of an index (and the identity key) are fetched: the
   engine needs them for images. */
void ha_vdb_mysql_storage::add_key_columns(uint idx) {
  auto add = [this](uint k) {
    if (k >= table->s->keys) return;
    KEY *key = &table->key_info[k];
    for (uint i = 0; i < key->user_defined_key_parts; i++) {
      uint c = key->key_part[i].fieldnr - 1;
      if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
    }
  };
  add(idx);
  if (identity != MAX_KEY) add(identity);
}

int ha_vdb_mysql_storage::scan_close() {
  if (scan_open && scan_id != 0) {
    int err = 0;
    if (Vdb_conn *c = conn(&err)) {
      vdb::ScanClose m;
      m.set_scan(scan_id);
      c->call(vdb_proto::OP_SCAN_CLOSE, m, nullptr, nullptr);
    }
  }
  scan_open = false;
  scan_id = 0;
  ordered = false;
  merged.clear();
  src.clear();
  dlt.clear();
  shadows.clear();
  lookup_cont.clear();
  have_src_head = have_dlt_head = false;
  prev_src.clear();
  prev_dlt.clear();
  return 0;
}

static void set_columns(vdb::ScanOpen *m, const std::vector<uint32_t> &cols) {
  for (uint32_t c : cols) m->add_columns(c);
}

int ha_vdb_mysql_storage::rnd_init(bool scan) {
  if (!scan) return 0;  // positioned reads only
  scan_close();
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
  pick_columns(false);
  if (mirrored) add_key_columns(MAX_KEY);
  vdb::ScanOpen m;
  m.set_table(tid);
  m.set_version(tversion);
  m.set_index(identity == MAX_KEY ? 0 : identity);
  m.set_kind(vdb::SCAN_UNORDERED);
  set_columns(&m, cols);
  m.set_pushed(pushed);
  m.set_lock(static_cast<vdb::LockMode>(lock_mode));
  vdb::ScanOpenReply r;
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_SCAN_OPEN, m, &r, &e);
  vdb_bump(vdb_st_requests);
  if (rc != 0) return fail(rc, e);
  scan_id = r.scan();
  scan_open = true;
  merged.batch = r.merged();
  merged.pos = 0;
  merged.end = r.merged().end();
  take_shadows(r.shadow_images(), &shadows);
  return 0;
}

int ha_vdb_mysql_storage::fetch_more(Vdb_rows *rows, vdb::CursorKind k) {
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
  vdb::Error e;
  if (!lookup_cont.empty() && k == vdb::CURSOR_MERGED && scan_id == 0) {
    vdb::LookupNext m;
    m.set_continuation(lookup_cont);
    vdb::LookupReply r;
    int rc = c->call(vdb_proto::OP_LOOKUP_NEXT, m, &r, &e);
    vdb_bump(vdb_st_requests);
    if (rc != 0) return fail(rc, e);
    rows->batch = r.batch();
    rows->pos = 0;
    lookup_cont = r.continuation();
    rows->end = lookup_cont.empty();
    return 0;
  }
  vdb::ScanNext m;
  m.set_scan(scan_id);
  m.set_cursor(k);
  vdb::ScanNextReply r;
  int rc = c->call(vdb_proto::OP_SCAN_NEXT, m, &r, &e);
  vdb_bump(vdb_st_requests);
  if (rc != 0) return fail(rc, e);
  rows->batch = r.batch();
  rows->pos = 0;
  rows->end = r.batch().end();
  return 0;
}

/* The next row of an unordered scan or a lookup: Source rows hidden by a
   collation-equal Delta key are dropped (doc 03 §2.4). */
int ha_vdb_mysql_storage::next_unordered(uchar *buf) {
  for (;;) {
    if (!merged.more()) {
      if (merged.end && lookup_cont.empty()) return HA_ERR_END_OF_FILE;
      if (int e = fetch_more(&merged, vdb::CURSOR_MERGED)) return e;
      if (!merged.more() && merged.end) return HA_ERR_END_OF_FILE;
      continue;
    }
    int i = merged.pos++;
    if (int e = decode_row(merged.batch.rows(i), buf)) return e;
    const vdb::RowRef &ref = merged.batch.refs(i);
    if (ref.origin() == vdb::ORIGIN_SOURCE && shadowed(buf)) continue;
    last_keyidx = i < merged.batch.key_idx_size() ? merged.batch.key_idx(i) : 0;
    remember(ref);
    return 0;
  }
}

int ha_vdb_mysql_storage::rnd_next(uchar *buf) {
  int e = next_unordered(buf);
  return e;
}

int ha_vdb_mysql_storage::rnd_end() { return scan_close(); }

/* A position is an index into this statement's saved rows: the row with
   every column and the reference update_row and delete_row send back. The
   same row always gets the same position, because mysqld compares positions
   to find duplicates (semi-join duplicate weedout, DISTINCT by row id).
   Rows of a table without a usable key (D-10) can be identical, so each of
   their calls gets a new position. */
void ha_vdb_mysql_storage::position(const uchar *record) {
  vdb::Row r;
  encode_row(record, &r);
  const std::string key = cur_ref.SerializeAsString();
  uint64_t n;
  const bool keyed = !cur_ref.has_whole() || !cur_ref.pk().image().empty();
  auto it = keyed ? pos_index.find(key) : pos_index.end();
  if (it != pos_index.end()) {
    n = it->second;
    pos_rows[n] = r.SerializeAsString();
  } else {
    n = pos_rows.size();
    pos_rows.push_back(r.SerializeAsString());
    pos_refs.push_back(cur_ref);
    if (keyed) pos_index.emplace(key, n);
  }
  memcpy(ref, &n, sizeof(n));
}

/* unlock_row: mysqld found that the row a locking read just returned does
   not match the WHERE, and asks the engine to release that row's lock
   (Source scaling plan 03, L3). vdb.so forwards the call for every row a
   locking read returned (a plain read asked for no lock, so there is none to
   name). vdb-mysql-engine decides whether anything is released, by
   InnoDB's rule (only at READ COMMITTED and READ UNCOMMITTED), and
   vdb-core's lock table keeps a lock the transaction held before the
   statement.
   unlock_row returns nothing, so a failed request is ignored: the lock then
   stays until commit, which is stricter, never wrong. */
void ha_vdb_mysql_storage::unlock_row() {
  if (lock_mode == vdb::LOCK_NONE) return;
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return;
  vdb::Unlock m;
  m.set_table(tid);
  *m.mutable_ref() = cur_ref;
  c->call(vdb_proto::OP_UNLOCK, m, nullptr, nullptr);
  vdb_bump(vdb_st_requests);
}

int ha_vdb_mysql_storage::rnd_pos(uchar *buf, uchar *pos) {
  uint64_t n;
  memcpy(&n, pos, sizeof(n));
  if (n >= pos_rows.size()) return HA_ERR_KEY_NOT_FOUND;
  vdb::Row r;
  r.ParseFromString(pos_rows[n]);
  std::vector<uint32_t> saved = cols;
  cols.clear();
  for (uint i = 0; i < table->s->fields; i++) cols.push_back(i);
  int e = decode_row(r, buf);
  cols = saved;
  remember(pos_refs[n]);
  return e;
}

int ha_vdb_mysql_storage::index_init(uint idx, bool) {
  scan_close();
  active_index = idx;
  return 0;
}

int ha_vdb_mysql_storage::index_end() {
  scan_close();
  active_index = MAX_KEY;
  mrr_own = false;
  return 0;
}

/* One Lookup: the rows answering keys on an index, then index_next. */
int ha_vdb_mysql_storage::lookup(uint idx, const std::vector<vdb::Key> &keys, uchar *buf) {
  scan_close();
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
  pick_columns(false);
  if (mirrored) add_key_columns(MAX_KEY);
  vdb::Lookup m;
  m.set_table(tid);
  m.set_version(tversion);
  m.set_index(idx);
  for (const auto &k : keys) *m.add_keys() = k;
  for (uint32_t col : cols) m.add_columns(col);
  m.set_lock(static_cast<vdb::LockMode>(lock_mode));
  vdb::LookupReply r;
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_LOOKUP, m, &r, &e);
  vdb_bump(vdb_st_requests);
  if (rc != 0) return fail(rc, e);
  merged.batch = r.batch();
  merged.pos = 0;
  lookup_cont = r.continuation();
  merged.end = lookup_cont.empty();
  take_shadows(r.shadow_images(), &shadows);
  if (buf == nullptr) return 0;
  return next_unordered(buf);
}

static void set_bound(vdb::Range *r, bool low, const vdb::Key &k, uint parts, bool incl) {
  if (low) {
    *r->mutable_low() = k;
    r->set_low_incl(incl);
    r->set_low_parts(parts);
  } else {
    *r->mutable_high() = k;
    r->set_high_incl(incl);
    r->set_high_parts(parts);
  }
}

int ha_vdb_mysql_storage::index_read_map(uchar *buf, const uchar *key, key_part_map kpm, enum ha_rkey_function f) {
  uint key_len;
  uint parts = parts_of(active_index, kpm, key, &key_len);
  KEY *k = &table->key_info[active_index];
  vdb::Key vk;
  make_range_key(active_index, key, key_len, parts, &vk);
  if (f == HA_READ_KEY_EXACT && (k->flags & HA_NOSAME) && parts == k->user_defined_key_parts) {
    std::vector<vdb::Key> keys{vk};
    int e = lookup(active_index, keys, buf);
    return e == HA_ERR_END_OF_FILE ? HA_ERR_KEY_NOT_FOUND : e;
  }
  vdb::Range r;
  bool back = false;
  switch (f) {
    case HA_READ_KEY_EXACT:
      set_bound(&r, true, vk, parts, true);
      set_bound(&r, false, vk, parts, true);
      break;
    case HA_READ_KEY_OR_NEXT:
      set_bound(&r, true, vk, parts, true);
      break;
    case HA_READ_AFTER_KEY:
      set_bound(&r, true, vk, parts, false);
      break;
    case HA_READ_BEFORE_KEY:
      set_bound(&r, false, vk, parts, false);
      back = true;
      break;
    case HA_READ_KEY_OR_PREV:
    case HA_READ_PREFIX_LAST_OR_PREV:
      set_bound(&r, false, vk, parts, true);
      back = true;
      break;
    case HA_READ_PREFIX_LAST:
      set_bound(&r, true, vk, parts, true);
      set_bound(&r, false, vk, parts, true);
      back = true;
      break;
    default:
      return HA_ERR_WRONG_COMMAND;
  }
  if (int e = open_ordered(&r, back)) return e;
  int e = next_ordered(buf);
  if (e == HA_ERR_END_OF_FILE && f == HA_READ_KEY_EXACT) return HA_ERR_KEY_NOT_FOUND;
  return e;
}

int ha_vdb_mysql_storage::index_read_last_map(uchar *buf, const uchar *key, key_part_map kpm) {
  return index_read_map(buf, key, kpm, HA_READ_PREFIX_LAST);
}

int ha_vdb_mysql_storage::index_first(uchar *buf) {
  if (int e = open_ordered(nullptr, false)) return e;
  return next_ordered(buf);
}

int ha_vdb_mysql_storage::index_last(uchar *buf) {
  if (int e = open_ordered(nullptr, true)) return e;
  return next_ordered(buf);
}

int ha_vdb_mysql_storage::index_next(uchar *buf) {
  if (ordered) {
    if (backward) return HA_ERR_WRONG_COMMAND;
    return next_ordered(buf);
  }
  return next_unordered(buf);
}

int ha_vdb_mysql_storage::index_prev(uchar *buf) {
  if (ordered && backward) return next_ordered(buf);
  return HA_ERR_WRONG_COMMAND;
}

int ha_vdb_mysql_storage::index_next_same(uchar *buf, const uchar *key, uint keylen) {
  int e = index_next(buf);
  if (e == 0 && key_cmp_if_same(table, key, active_index, keylen)) return HA_ERR_END_OF_FILE;
  return e;
}

/* A range read sends both bounds, so the Source reads only the range. */
int ha_vdb_mysql_storage::read_range_first(const key_range *start, const key_range *end, bool eq, bool sorted) {
  KEY *k = &table->key_info[active_index];
  if (eq && start && (k->flags & HA_NOSAME)) return handler::read_range_first(start, end, eq, sorted);
  eq_range = eq;
  set_end_range(end, RANGE_SCAN_ASC);
  range_key_part = k->key_part;
  vdb::Range r;
  if (start) {
    vdb::Key vk;
    uint len;
    uint parts = parts_of(active_index, start->keypart_map, start->key, &len);
    make_range_key(active_index, start->key, len, parts, &vk);
    set_bound(&r, true, vk, parts, start->flag != HA_READ_AFTER_KEY);
  }
  if (end) {
    vdb::Key vk;
    uint len;
    uint parts = parts_of(active_index, end->keypart_map, end->key, &len);
    make_range_key(active_index, end->key, len, parts, &vk);
    set_bound(&r, false, vk, parts, end->flag == HA_READ_AFTER_KEY);
  }
  if (int e = open_ordered(&r, false)) return e;
  int e = next_ordered(table->record[0]);
  if (e == 0 && end_range && compare_key(end_range) > 0) return HA_ERR_END_OF_FILE;
  return e;
}
