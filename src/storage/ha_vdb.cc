/* ha_vdb.cc: the handler class (doc 08 §3.1): open sends TableOpen and keeps
   the table id, version and flags; table and index flags; external_lock and
   start_stmt send StmtBegin and register with the transaction coordinator;
   store_lock keeps MySQL's table lock levels (row locks are vdb-core's). */
#include "storage/vdb/storage/ha_vdb.h"

#include "my_dbug.h"
#include "mysql/plugin.h"
#include "sql/field.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/table.h"
#include "storage/vdb/common/errors.h"
#include "storage/vdb/common/session.h"

unsigned long long vdb_st_requests, vdb_st_rows_received, vdb_st_rows_sent, vdb_st_bytes_received, vdb_st_lock_waits,
    vdb_st_order_violations;

void vdb_report_wait(THD *thd, uint64_t holder);                                     // lockwait.cc
void vdb_compute_images(THD *thd, const vdb::ImagesNeeded &q, vdb::ImagesReply *r);  // images.cc

handler *vdb_create_handler(handlerton *hton, TABLE_SHARE *table, bool, MEM_ROOT *mem_root) {
  return new (mem_root) ha_vdb_mysql_storage(hton, table);
}

ha_vdb_mysql_storage::ha_vdb_mysql_storage(handlerton *hton, TABLE_SHARE *s) : handler(hton, s) {
  ref_length = 8;  // set in open(), from the table's keys
}

ha_vdb_mysql_storage::~ha_vdb_mysql_storage() {
  my_free(scratch);
  my_free(src_head);
  my_free(dlt_head);
}

/* The primary key or identity is the row reference; ordered and reverse
   index reads only on indexes whose collation is proven for order (D-59). */
ulonglong ha_vdb_mysql_storage::table_flags() const {
  return HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE | HA_PARTIAL_COLUMN_READ | HA_NULL_IN_KEY | HA_CAN_INDEX_BLOBS |
         HA_GENERATED_COLUMNS;
}

ulong ha_vdb_mysql_storage::index_flags(uint idx, uint, bool) const {
  ulong f = HA_READ_NEXT;
  if (idx < idx_ordered.size() ? idx_ordered[idx] : true) f |= HA_READ_ORDER | HA_READ_PREV | HA_READ_RANGE;
  return f;
}

Vdb_conn *ha_vdb_mysql_storage::conn(int *err) {
  Vdb_conn *c = vdb_data_conn(ha_thd(), &err_msg);
  if (c == nullptr) {
    *err = HA_ERR_VDB;
    return nullptr;
  }
  wire_callbacks(c);
  return c;
}

/* Lock waits are reported to mysqld; cascades ask for sort images (D-86). */
void ha_vdb_mysql_storage::wire_callbacks(Vdb_conn *c) {
  THD *thd = ha_thd();
  c->on_wait = [thd](uint64_t holder) {
    vdb_bump(vdb_st_lock_waits);
    vdb_report_wait(thd, holder);
  };
  c->on_images = [thd](const vdb::ImagesNeeded &q, vdb::ImagesReply *r) { vdb_compute_images(thd, q, r); };
}

/* Turn an error reply into a handler error, keeping the message. */
int ha_vdb_mysql_storage::fail(int rc, const vdb::Error &e) {
  if (rc < 0) return vdb_unavailable(&err_msg);
  unsigned key = 0;
  int err = vdb_ha_error(e, &err_msg, &key);
  if (err == HA_ERR_FOUND_DUPP_KEY) errkey = dup_key = key;
  return err;
}

bool ha_vdb_mysql_storage::get_error_message(int, String *buf) {
  buf->copy(err_msg.c_str(), err_msg.size(), system_charset_info);
  return false;
}

/* TableOpen: the table id, version, kind, index flags, fetch modes. */
int ha_vdb_mysql_storage::table_open() {
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
  vdb::TableOpen m;
  m.set_database(table_share->db.str, table_share->db.length);
  m.set_table(table_share->table_name.str, table_share->table_name.length);
  vdb::TableOpenReply r;
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_TABLE_OPEN, m, &r, &e);
  vdb_bump(vdb_st_requests);
  if (rc != 0) return fail(rc, e);
  tid = r.table();
  tversion = r.version();
  mirrored = r.kind() == vdb::KIND_MIRRORED;
  identity = r.identity() == 0xFFFF ? MAX_KEY : r.identity();
  idx_ordered.assign(table_share->keys, true);
  idx_eq.assign(table_share->keys, true);
  for (const auto &ix : r.indexes()) {
    if (ix.id() < idx_ordered.size()) {
      idx_ordered[ix.id()] = ix.ordered();
      idx_eq[ix.id()] = ix.equality_proven();
    }
  }
  fetch.assign(r.fetch().begin(), r.fetch().end());
  return 0;
}

int ha_vdb_mysql_storage::open(const char *, int, uint, const dd::Table *) {
  lock_shared_ha_data();
  share = static_cast<Vdb_share *>(get_ha_share_ptr());
  if (share == nullptr) {
    share = new Vdb_share();
    set_ha_share_ptr(share);
  }
  unlock_shared_ha_data();
  thr_lock_data_init(&share->lock, &lock_data, nullptr);
  scratch = static_cast<uchar *>(my_malloc(PSI_NOT_INSTRUMENTED, table_share->rec_buff_length + 1, MYF(MY_WME)));
  if (scratch == nullptr) return HA_ERR_OUT_OF_MEM;
  src_head = static_cast<uchar *>(my_malloc(PSI_NOT_INSTRUMENTED, table_share->rec_buff_length + 1, MYF(MY_WME)));
  dlt_head = static_cast<uchar *>(my_malloc(PSI_NOT_INSTRUMENTED, table_share->rec_buff_length + 1, MYF(MY_WME)));
  if (src_head == nullptr || dlt_head == nullptr) return HA_ERR_OUT_OF_MEM;
  memcpy(src_head, table_share->default_values, table_share->reclength);
  memcpy(dlt_head, table_share->default_values, table_share->reclength);
  /* A position is the row's identity key in MySQL's key format (scan.cc).
     mysqld allocates ref right after open(), before vdb-mysql names the
     identity (TableOpen comes at the first statement), so ref_length fits
     the longest unique key; a keyless table's position is 8 bytes. */
  ref_length = 8;
  for (uint k = 0; k < table_share->keys; k++)
    if ((table_share->key_info[k].flags & HA_NOSAME) && table_share->key_info[k].key_length > ref_length)
      ref_length = table_share->key_info[k].key_length;
  /* TableOpen is sent at the first statement: open may run where vdb-mysql
     does not know the table yet (during CREATE TABLE). */
  return 0;
}

int ha_vdb_mysql_storage::close() {
  scan_close();
  blob_store.clear();
  return 0;
}

int ha_vdb_mysql_storage::extra(enum ha_extra_function) { return 0; }

int ha_vdb_mysql_storage::reset() {
  pushed.clear();
  pos_rows.clear();
  pos_refs.clear();
  return 0;
}

/* Statement start: StmtBegin once per statement, the transaction
   registration, the lock mode, and TableOpen on first use. */
int ha_vdb_mysql_storage::begin(THD *thd) {
  if (int e = vdb_begin_statement(thd, &err_msg)) return e;
  if (tid == 0) return table_open();
  return 0;
}

int ha_vdb_mysql_storage::external_lock(THD *thd, int lock_type) {
  if (lock_type == F_UNLCK) {
    if (bulk) flush_bulk();
    return 0;
  }
  lock_mode = vdb::LOCK_NONE;
  if (lock_type == F_WRLCK)
    lock_mode = vdb::LOCK_EXCLUSIVE;
  else if (table->reginfo.lock_type == TL_READ_WITH_SHARED_LOCKS)
    lock_mode = vdb::LOCK_SHARED;
  return begin(thd);
}

int ha_vdb_mysql_storage::start_stmt(THD *thd, thr_lock_type lt) {
  lock_mode = lt >= TL_WRITE_ALLOW_WRITE        ? vdb::LOCK_EXCLUSIVE
              : lt == TL_READ_WITH_SHARED_LOCKS ? vdb::LOCK_SHARED
                                                : vdb::LOCK_NONE;
  return begin(thd);
}

THR_LOCK_DATA **ha_vdb_mysql_storage::store_lock(THD *, THR_LOCK_DATA **to, enum thr_lock_type lt) {
  if (lt != TL_IGNORE && lock_data.type == TL_UNLOCK) {
    /* Row locks are vdb-core's: let concurrent writers in, as InnoDB does. */
    if (lt >= TL_WRITE_CONCURRENT_INSERT && lt <= TL_WRITE) lt = TL_WRITE_ALLOW_WRITE;
    if (lt == TL_READ_NO_INSERT) lt = TL_READ;
    lock_data.type = lt;
  }
  *to++ = &lock_data;
  return to;
}

/* Columns to fetch: the read set; every column for statements that write the
   row back (the Delta holds whole rows, D-53), minus virtual columns. */
void ha_vdb_mysql_storage::pick_columns(bool all) {
  THD *thd = ha_thd();
  switch (thd_sql_command(thd)) {
    case SQLCOM_UPDATE:
    case SQLCOM_UPDATE_MULTI:
    case SQLCOM_DELETE:
    case SQLCOM_DELETE_MULTI:
    case SQLCOM_REPLACE:
    case SQLCOM_REPLACE_SELECT:
    case SQLCOM_INSERT:
    case SQLCOM_INSERT_SELECT:
    case SQLCOM_LOAD:
      all = true;
      break;
    default:
      break;
  }
  cols.clear();
  for (uint i = 0; i < table_share->fields; i++) {
    Field *f = table->field[i];
    if (f->is_virtual_gcol()) continue;
    if (all || bitmap_is_set(table->read_set, i) || bitmap_is_set(table->write_set, i)) {
      cols.push_back(i);
      /* Say the column was read: with HA_PARTIAL_COLUMN_READ, mysqld only
         compares old and new rows (and reports "0 rows changed") when every
         written column is in read_set. */
      bitmap_set_bit(table->read_set, i);
    }
  }
  /* The identity columns too: a position is the identity key (scan.cc). */
  if (identity != MAX_KEY) add_key_columns(MAX_KEY);
}
