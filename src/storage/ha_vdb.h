/* ha_vdb.h: the vdb_mysql_storage handler (implementation plan doc 08 §3).
   Every table of VDB's mysqld uses it. A handler keeps only its table's id
   and flags from vdb-mysql, the current scan's batches, and the positions of
   the current statement; everything else lives in vdb-mysql (product doc 03
   §1, rule 5). */
#ifndef VDB_STORAGE_HA_VDB_H
#define VDB_STORAGE_HA_VDB_H

#include <string>
#include <unordered_map>
#include <vector>

#include "my_base.h"
#include "sql/handler.h"
#include "storage/vdb/common/conn.h"
#include "thr_lock.h"

/* Shared per table: MySQL's table lock structure. */
class Vdb_share : public Handler_share {
 public:
  THR_LOCK lock;
  Vdb_share() { thr_lock_init(&lock); }
  ~Vdb_share() override { thr_lock_delete(&lock); }
};

/* One cursor's batch from vdb-mysql. */
struct Vdb_rows {
  vdb::Batch batch;
  int pos = 0;
  bool end = true;
  bool more() const { return pos < batch.rows_size(); }
  void clear() {
    batch.Clear();
    pos = 0;
    end = true;
  }
};

/* Engine status counters (SHOW GLOBAL STATUS LIKE 'vdb%'). */
extern unsigned long long vdb_st_requests, vdb_st_rows_received, vdb_st_rows_sent, vdb_st_bytes_received,
    vdb_st_lock_waits, vdb_st_order_violations;
inline void vdb_bump(unsigned long long &c, unsigned long long n = 1) { __atomic_add_fetch(&c, n, __ATOMIC_RELAXED); }

class Field_blob;

class ha_vdb_mysql_storage : public handler {
 public:
  ha_vdb_mysql_storage(handlerton *hton, TABLE_SHARE *share);
  ~ha_vdb_mysql_storage() override;

  /* ── ha_vdb.cc ── */
  const char *table_type() const override { return "vdb_mysql_storage"; }
  enum ha_key_alg get_default_index_algorithm() const override { return HA_KEY_ALG_BTREE; }
  bool is_index_algorithm_supported(enum ha_key_alg a) const override { return a == HA_KEY_ALG_BTREE; }
  ulonglong table_flags() const override;
  ulong index_flags(uint idx, uint part, bool all_parts) const override;
  uint max_supported_record_length() const override { return HA_MAX_REC_LENGTH; }
  uint max_supported_keys() const override { return MAX_KEY; }
  uint max_supported_key_parts() const override { return MAX_REF_PARTS; }
  uint max_supported_key_length() const override { return 3072; }
  uint max_supported_key_part_length(HA_CREATE_INFO *) const override { return 3072; }
  int open(const char *name, int mode, uint test_if_locked, const dd::Table *) override;
  int close() override;
  int extra(enum ha_extra_function op) override;
  int reset() override;
  int external_lock(THD *thd, int lock_type) override;
  int start_stmt(THD *thd, thr_lock_type lock_type) override;
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lt) override;
  bool get_error_message(int error, String *buf) override;
  const handlerton *hton_supporting_engine_pushdown() override { return ht; }

  /* ── scan.cc ── */
  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_end() override;
  void position(const uchar *record) override;
  void unlock_row() override;
  int rnd_pos(uchar *buf, uchar *pos) override;
  int read_position(uchar *buf, const uchar *pos);
  int read_saved(uchar *buf, const uchar *pos);
  int index_init(uint idx, bool sorted) override;
  int index_end() override;
  int index_read_map(uchar *buf, const uchar *key, key_part_map kpm, enum ha_rkey_function f) override;
  int index_read_last_map(uchar *buf, const uchar *key, key_part_map kpm) override;
  int index_next(uchar *buf) override;
  int index_prev(uchar *buf) override;
  int index_first(uchar *buf) override;
  int index_last(uchar *buf) override;
  int index_next_same(uchar *buf, const uchar *key, uint keylen) override;
  int read_range_first(const key_range *start_key, const key_range *end_key, bool eq_range, bool sorted) override;

  /* ── mrr.cc ── */
  ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq, void *seq_init_param, uint n_ranges, uint *bufsz,
                                      uint *flags, bool *force_default_mrr, Cost_estimate *cost) override;
  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys, uint *bufsz, uint *flags,
                                Cost_estimate *cost) override;
  int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param, uint n_ranges, uint mode,
                            HANDLER_BUFFER *buf) override;
  int multi_range_read_next(char **range_info) override;

  /* ── cond.cc ── */
  const Item *cond_push(const Item *cond) override;
  Item *idx_cond_push(uint keyno, Item *idx_cond) override;
  void push_scan_condition(THD *thd, const Item *cond);

  /* ── write.cc ── */
  int write_row(uchar *buf) override;
  int update_row(const uchar *old_data, uchar *new_data) override;
  int delete_row(const uchar *buf) override;
  void start_bulk_insert(ha_rows rows) override;
  int end_bulk_insert() override;
  void get_auto_increment(ulonglong offset, ulonglong increment, ulonglong nb_desired, ulonglong *first,
                          ulonglong *nb_reserved) override;

  /* ── ddl.cc ── */
  int create(const char *name, TABLE *form, HA_CREATE_INFO *info, dd::Table *dd) override;
  int delete_table(const char *name, const dd::Table *dd) override;
  int rename_table(const char *from, const char *to, const dd::Table *from_dd, dd::Table *to_dd) override;
  int truncate(dd::Table *dd) override;
  int delete_all_rows() override;
  enum_alter_inplace_result check_if_supported_inplace_alter(TABLE *altered, Alter_inplace_info *ai) override;
  bool prepare_inplace_alter_table(TABLE *altered, Alter_inplace_info *ai, const dd::Table *old_dd,
                                   dd::Table *new_dd) override;
  bool inplace_alter_table(TABLE *altered, Alter_inplace_info *ai, const dd::Table *old_dd, dd::Table *new_dd) override;
  bool commit_inplace_alter_table(TABLE *altered, Alter_inplace_info *ai, bool commit, const dd::Table *old_dd,
                                  dd::Table *new_dd) override;

  /* ── stats.cc ── */
  int info(uint flag) override;
  int records(ha_rows *num_rows) override;
  ha_rows records_in_range(uint inx, key_range *min_key, key_range *max_key) override;
  double scan_time() override;
  double read_time(uint index, uint ranges, ha_rows rows) override;

  /* State shared by the files of this handler. */
  uint32_t tid = 0;       // table id in vdb-mysql
  uint64_t tversion = 0;  // catalog version at open
  uint identity = MAX_KEY;
  bool mirrored = false;
  std::vector<bool> idx_ordered, idx_eq;
  std::vector<int> fetch;  // vdb::FetchMode per column
  std::string err_msg;
  uint dup_key{MAX_KEY};  // the key of the last duplicate-key error
  int lock_mode = 0;      // vdb::LockMode of the current statement
  std::string pushed;

  /* The current scan. */
  uint32_t scan_id = 0;
  bool scan_open = false;
  bool ordered = false;
  bool backward = false;
  Vdb_rows merged, src, dlt;
  std::vector<std::string> shadows;  // sorted identity images
  std::vector<uint32_t> cols;        // requested columns
  std::string prev_src, prev_dlt;    // last images, for the order check
  bool have_src_head = false, have_dlt_head = false;
  std::string src_img, dlt_img;
  uchar *src_head = nullptr, *dlt_head = nullptr;  // each cursor's next row, decoded once
  std::string lookup_cont;
  uint32_t last_keyidx = 0;
  double batch_cost = 1.0;
  bool convert = false;  // the current ALTER converts Delta rows
  void add_key_columns(uint idx);

  /* The row last returned: what update_row and delete_row send back. */
  vdb::RowRef cur_ref;

  /* Positions of this statement in a keyless table: a record copy and its
     reference (a keyed table's position is its identity key). */
  std::vector<std::string> pos_rows;
  std::vector<vdb::RowRef> pos_refs;

  /* Bulk insert and MRR. */
  bool bulk = false;
  vdb::InsertBatch bulk_req;
  bool mrr_own = false;
  std::vector<char *> mrr_ptrs;

  uchar *scratch = nullptr;  // a record buffer for keys and images

  /* Values of BLOB, TEXT and JSON fields, per record buffer and field. */
  std::unordered_map<const uchar *, std::vector<std::string>> blob_store;

  /* ── helpers (ha_vdb.cc, rows.cc, keys.cc, scan.cc) ── */
  Vdb_conn *conn(int *err);
  int begin(THD *thd);
  int detach(uint8_t op, const google::protobuf::MessageLite &m, google::protobuf::MessageLite *reply);
  int fail(int rc, const vdb::Error &e);
  int table_open();
  void pick_columns(bool all);
  void wire_callbacks(Vdb_conn *c);
  int decode_row(const vdb::Row &row, uchar *buf);
  void keep_blob(Field_blob *b, const uchar *buf, uint32_t field);
  void copy_row(const uchar *from, uchar *buf);
  void detach_blob(Field_blob *b, uint32_t field);
  void encode_row(const uchar *rec, vdb::Row *out);
  void encode_fields(const uchar *rec, const std::vector<Field *> &fs, vdb::Row *out);
  std::string key_image(uint idx, const uchar *rec, uint parts);
  void row_images(const uchar *rec, vdb::Images *out);
  std::string key_raw(uint idx, const uchar *rec, uint parts);
  uint parts_of(uint idx, key_part_map kpm, const uchar *key, uint *key_len);
  int scan_close();
  int open_ordered(const vdb::Range *range, bool back);
  int next_unordered(uchar *buf);
  int next_ordered(uchar *buf);
  int fetch_more(Vdb_rows *r, vdb::CursorKind k);
  int lookup(uint idx, const std::vector<vdb::Key> &keys, uchar *buf);
  bool shadowed(const uchar *rec);
  int flush_bulk();
  void remember(const vdb::RowRef &ref) { cur_ref = ref; }
  void make_range_key(uint idx, const uchar *key, uint key_len, uint parts, vdb::Key *out);

 private:
  THR_LOCK_DATA lock_data;
  Vdb_share *share = nullptr;
};

/* Every handler of this engine: used by images.cc to find an open table. */
handler *vdb_create_handler(handlerton *hton, TABLE_SHARE *table, bool partitioned, MEM_ROOT *mem_root);

#endif
