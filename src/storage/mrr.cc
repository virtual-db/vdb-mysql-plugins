/* mrr.cc: VDB's own multi-range read for Batched Key Access (doc 03 §2.2):
   one Lookup per join buffer of keys, each row tagged with the key it
   answers. Range scans keep MySQL's default implementation. */
#include "sql/handler.h"
#include "sql/table.h"
#include "storage/vdb/storage/ha_vdb.h"

ha_rows ha_vdb_mysql_storage::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq, void *seq_init_param,
                                                          uint n_ranges, uint *bufsz, uint *flags,
                                                          bool *force_default_mrr, Cost_estimate *cost) {
  return handler::multi_range_read_info_const(keyno, seq, seq_init_param, n_ranges, bufsz, flags, force_default_mrr,
                                              cost);
}

ha_rows ha_vdb_mysql_storage::multi_range_read_info(uint keyno, uint n_ranges, uint keys, uint *bufsz, uint *flags,
                                                    Cost_estimate *cost) {
  ha_rows r = handler::multi_range_read_info(keyno, n_ranges, keys, bufsz, flags, cost);
  *flags &= ~(HA_MRR_USE_DEFAULT_IMPL | HA_MRR_SUPPORT_SORTED);
  return r;
}

int ha_vdb_mysql_storage::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param, uint n_ranges, uint mode,
                                                HANDLER_BUFFER *buf) {
  if (mode & HA_MRR_USE_DEFAULT_IMPL) {
    mrr_own = false;
    return handler::multi_range_read_init(seq, seq_init_param, n_ranges, mode, buf);
  }
  mrr_own = true;
  mrr_ptrs.clear();
  std::vector<vdb::Key> keys;
  range_seq_t it = seq->init(seq_init_param, n_ranges, mode);
  KEY_MULTI_RANGE range;
  while (!seq->next(it, &range)) {
    uint len;
    uint parts = parts_of(active_index, range.start_key.keypart_map, range.start_key.key, &len);
    vdb::Key k;
    make_range_key(active_index, range.start_key.key, len, parts, &k);
    keys.push_back(k);
    mrr_ptrs.push_back(range.ptr);
  }
  if (keys.empty()) {
    scan_close();
    merged.clear();
    return 0;
  }
  return lookup(active_index, keys, nullptr);
}

int ha_vdb_mysql_storage::multi_range_read_next(char **range_info) {
  if (!mrr_own) return handler::multi_range_read_next(range_info);
  int e = next_unordered(table->record[0]);
  if (e == 0 && range_info && last_keyidx < mrr_ptrs.size()) *range_info = mrr_ptrs[last_keyidx];
  return e;
}
