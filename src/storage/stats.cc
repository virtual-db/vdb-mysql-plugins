/* stats.cc: statistics (product doc 03 §2.5, §2.6). Every statistic the
   optimizer reads comes from vdb-mysql (doc 04 §5); records() answers
   COUNT(*) with vdb-core's exact count, or counts rows when it declines. In
   the debug build each statistic is poisoned before info fills it, and an
   assertion fails if one is left. */
#include <cassert>

#include "sql/handler.h"
#include "sql/key.h"
#include "sql/table.h"
#include "storage/vdb/common/errors.h"
#include "storage/vdb/storage/ha_vdb.h"

namespace {
#ifndef NDEBUG
constexpr ha_rows kPoison = 0xDEADBEEF;
#endif
}  // namespace

int ha_vdb_mysql_storage::info(uint flag) {
  /* get_dup_key() resets errkey before asking, so set it from the reply. */
  if (flag & HA_STATUS_ERRKEY) {
    errkey = dup_key;
    return 0;
  }
  if (!(flag & (HA_STATUS_VARIABLE | HA_STATUS_CONST))) return 0;
  if (tid == 0) {
    if (int e = table_open()) return e;
  }
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
#ifndef NDEBUG
  stats.records = kPoison;
  stats.mean_rec_length = static_cast<ulong>(kPoison);
  stats.mrr_length_per_rec = static_cast<uint>(kPoison);
#endif
  vdb::Stats m;
  m.set_table(tid);
  vdb::StatsReply r;
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_STATS, m, &r, &e);
  vdb_bump(vdb_st_requests);
  if (rc != 0) return fail(rc, e);
  stats.records = r.records() < 2 ? 2 : r.records();  // never a const table
  stats.mean_rec_length = static_cast<ulong>(r.mean_rec_length());
  stats.mrr_length_per_rec = static_cast<uint>(r.mrr_length_per_rec() + sizeof(void *));
  stats.data_file_length = r.data_file_length() ? r.data_file_length() : stats.records * stats.mean_rec_length;
  stats.deleted = 0;
  batch_cost = r.batch_cost() > 0 ? r.batch_cost() : 1.0;
  for (const auto &ix : r.indexes()) {
    if (ix.index() >= table->s->keys) continue;
    KEY *key = &table->key_info[ix.index()];
    for (int j = 0; j < ix.rec_per_key_size() && j < static_cast<int>(key->user_defined_key_parts); j++) {
      rec_per_key_t rpk = static_cast<rec_per_key_t>(ix.rec_per_key(j));
      if (rpk < 1.0f) rpk = 1.0f;
      key->set_records_per_key(j, rpk);
      key->rec_per_key[j] = static_cast<ulong>(rpk);
    }
  }
#ifndef NDEBUG
  assert(stats.records != kPoison);
  assert(stats.mean_rec_length != static_cast<ulong>(kPoison));
  assert(stats.mrr_length_per_rec != static_cast<uint>(kPoison));
#endif
  return 0;
}

int ha_vdb_mysql_storage::records(ha_rows *num_rows) {
  if (int e = begin(ha_thd())) return e;
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
  vdb::Count m;
  m.set_table(tid);
  vdb::CountReply r;
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_COUNT, m, &r, &e);
  vdb_bump(vdb_st_requests);
  if (rc != 0) return fail(rc, e);
  if (r.declined()) return handler::records(num_rows);
  *num_rows = r.count();
  return 0;
}

ha_rows ha_vdb_mysql_storage::records_in_range(uint inx, key_range *min_key, key_range *max_key) {
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr || tid == 0) return 10;
  vdb::Stats m;
  m.set_table(tid);
  m.set_index(inx);
  vdb::Range *r = m.mutable_range();
  uint saved = active_index;
  active_index = inx;
  if (min_key) {
    uint len;
    uint parts = parts_of(inx, min_key->keypart_map, min_key->key, &len);
    make_range_key(inx, min_key->key, len, parts, r->mutable_low());
    r->set_low_incl(min_key->flag != HA_READ_AFTER_KEY);
    r->set_low_parts(parts);
  }
  if (max_key) {
    uint len;
    uint parts = parts_of(inx, max_key->keypart_map, max_key->key, &len);
    make_range_key(inx, max_key->key, len, parts, r->mutable_high());
    r->set_high_incl(max_key->flag == HA_READ_AFTER_KEY);
    r->set_high_parts(parts);
  }
  active_index = saved;
  vdb::StatsReply rep;
  vdb::Error e;
  if (c->call(vdb_proto::OP_STATS, m, &rep, &e) != 0) return 10;
  return rep.records_in_range() ? rep.records_in_range() : 1;
}

/* The cost of a scan: batches of rows, each a measured round trip. */
double ha_vdb_mysql_storage::scan_time() { return static_cast<double>(stats.records) / 4096.0 * batch_cost + 2; }

double ha_vdb_mysql_storage::read_time(uint, uint ranges, ha_rows rows) {
  return static_cast<double>(ranges) * batch_cost + static_cast<double>(rows) / 1000.0;
}
