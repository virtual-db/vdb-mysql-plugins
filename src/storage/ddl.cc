/* ddl.cc: DDL on VDB tables (implementation plan doc 09 §3, §5.1).
   create, delete_table and rename_table forward to the catalog of record;
   truncate detaches the table; ALTERs that change only metadata are instant;
   ALTERs that change columns or keys convert the Delta rows in place with
   MySQL's own Field::store, through AlterRows and AlterPut; ALTERs that need
   a table copy are refused (D-45). Whether a change is allowed is decided in
   Go (vdb-mysql/internal/engine); the definition text is mysqld's own. */
#include <algorithm>
#include <cstring>

#include "sql/create_field.h"
#include "sql/dd/types/column.h"
#include "sql/dd/types/foreign_key.h"
#include "sql/dd/types/foreign_key_element.h"
#include "sql/dd/types/table.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/sql_alter.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_show.h"
#include "sql/sql_table.h"
#include "sql/table.h"
#include "storage/vdb/common/errors.h"
#include "storage/vdb/common/session.h"
#include "storage/vdb/storage/ha_vdb.h"
#include "storage/vdb/storage/vdb_util.h"

namespace {

uint32_t rule(dd::Foreign_key::enum_rule r) {
  switch (r) {
    case dd::Foreign_key::RULE_NO_ACTION:
      return 1;
    case dd::Foreign_key::RULE_CASCADE:
      return 2;
    case dd::Foreign_key::RULE_SET_NULL:
      return 3;
    default:
      return 0;
  }
}

/* Split "./db/table" into database and table names. */
void split_path(const char *path, std::string *db, std::string *table) {
  std::string p(path);
  size_t slash = p.rfind('/');
  std::string tname = slash == std::string::npos ? p : p.substr(slash + 1);
  std::string rest = slash == std::string::npos ? "" : p.substr(0, slash);
  size_t s2 = rest.rfind('/');
  std::string dname = s2 == std::string::npos ? rest : rest.substr(s2 + 1);
  char buf[NAME_LEN + 1];
  filename_to_tablename(dname.c_str(), buf, sizeof(buf));
  *db = buf;
  filename_to_tablename(tname.c_str(), buf, sizeof(buf));
  *table = buf;
}

bool is_tmp_name(const std::string &t) { return t.compare(0, 4, "#sql") == 0; }

int send_catalog(THD *thd, const vdb::Catalog &m, std::string *err_msg) {
  if (int e = vdb_begin_statement(thd, err_msg)) return e;
  Vdb_conn *c = vdb_data_conn(thd, err_msg);
  if (c == nullptr) return HA_ERR_VDB;
  vdb::CatalogReply r;
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_CATALOG, m, &r, &e);
  if (rc < 0) return vdb_unavailable(err_msg);
  if (rc > 0) return vdb_ha_error(e, err_msg, nullptr);
  return 0;
}

}  // namespace

/* The structured definition of t, with mysqld's own CREATE TABLE text. */
void vdb_table_definition(THD *thd, TABLE *t, const dd::Table *dd, vdb::TableDefinition *out) {
  TABLE_SHARE *s = t->s;
  out->set_database(s->db.str, s->db.length);
  out->set_name(s->table_name.str, s->table_name.length);
  Table_ref tl(t, s->db.str, s->db.length, s->table_name.str, s->table_name.length, s->table_name.str, TL_READ);
  String text;
  if (!store_create_info(thd, &tl, &text, nullptr, false, true) && text.length() > 0)
    out->set_definition(text.ptr(), text.length());
  my_bitmap_map *old = dbug_tmp_use_all_columns(t, t->read_set);
  ptrdiff_t defoff = s->default_values - t->record[0];
  for (uint i = 0; i < s->fields; i++) {
    Field *f = t->field[i];
    vdb::ColumnDef *c = out->add_columns();
    c->set_name(f->field_name);
    char tbuf[MAX_FIELD_WIDTH];
    String type(tbuf, sizeof(tbuf), system_charset_info);
    type.length(0);
    f->sql_type(type);
    if (type.length() > 0) c->set_type(type.ptr(), type.length());
    if (f->has_charset()) {
      c->set_charset(f->charset()->csname);
      c->set_collation(f->charset()->m_coll_name);
    } else if (f->charset() == &my_charset_bin && f->result_type() == STRING_RESULT) {
      c->set_charset("binary");
    }
    c->set_nullable(f->is_nullable());
    c->set_virtual_gen(f->is_virtual_gcol());
    c->set_timestamp(f->type() == MYSQL_TYPE_TIMESTAMP);
    c->set_float_(f->type() == MYSQL_TYPE_FLOAT || f->type() == MYSQL_TYPE_DOUBLE);
    c->set_auto_increment(f->is_flag_set(AUTO_INCREMENT_FLAG));
    if (!f->is_flag_set(NO_DEFAULT_VALUE_FLAG) && f->m_default_val_expr == nullptr && !f->is_virtual_gcol()) {
      c->set_has_default(true);
      f->move_field_offset(defoff);
      vdb_value_of(f, c->mutable_default_value());
      f->move_field_offset(-defoff);
    }
  }
  for (uint k = 0; k < s->keys; k++) {
    KEY *key = &t->key_info[k];
    vdb::IndexDef *ix = out->add_indexes();
    ix->set_name(key->name);
    ix->set_primary(k == s->primary_key);
    ix->set_unique(key->flags & HA_NOSAME);
    ix->set_fulltext_or_spatial(key->flags & (HA_FULLTEXT | HA_SPATIAL));
    for (uint j = 0; j < key->user_defined_key_parts; j++) {
      ix->add_columns(key->key_part[j].fieldnr - 1);
      ix->add_part_lens(static_cast<uint32_t>(vdb_part_len(&key->key_part[j])));
      ix->add_collations(key->key_part[j].field->has_charset() ? key->key_part[j].field->charset()->m_coll_name : "");
    }
  }
  dbug_tmp_restore_column_map(t->read_set, old);
  if (dd == nullptr) return;
  for (const dd::Foreign_key *fk : dd->foreign_keys()) {
    vdb::ForeignKeyDef *f = out->add_foreign_keys();
    f->set_name(fk->name().c_str());
    f->set_parent_db(fk->referenced_table_schema_name().c_str());
    f->set_parent_table(fk->referenced_table_name().c_str());
    f->set_on_update(rule(fk->update_rule()));
    f->set_on_delete(rule(fk->delete_rule()));
    std::vector<uint32_t> cols;
    for (const dd::Foreign_key_element *e : fk->elements()) {
      for (uint i = 0; i < s->fields; i++) {
        if (my_strcasecmp(system_charset_info, t->field[i]->field_name, e->column().name().c_str()) == 0) {
          f->add_columns(i);
          cols.push_back(i);
        }
      }
      f->add_parent_columns(e->referenced_column_name().c_str());
    }
    /* The child's supporting index: the first whose leading columns are these. */
    for (uint k = 0; k < s->keys; k++) {
      KEY *key = &t->key_info[k];
      if (key->user_defined_key_parts < cols.size()) continue;
      bool ok = true;
      for (size_t j = 0; j < cols.size(); j++)
        if (static_cast<uint32_t>(key->key_part[j].fieldnr) - 1 != cols[j]) ok = false;
      if (ok) {
        f->set_child_index(k);
        break;
      }
    }
  }
}

/* CREATE TABLE: refused for the intermediate table of a copying ALTER (D-45);
   otherwise the definition goes to the catalog of record. */
int ha_vdb_mysql_storage::create(const char *name, TABLE *form, HA_CREATE_INFO *, dd::Table *dd) {
  THD *thd = ha_thd();
  std::string db, tname;
  split_path(name, &db, &tname);
  if (is_tmp_name(tname)) {
    err_msg =
        "VDB: this ALTER TABLE needs a table copy, which would move the whole Source table into the Delta; "
        "VDB refuses it (D-45)";
    return HA_ERR_VDB;
  }
  vdb::Catalog m;
  m.set_kind(vdb::CATALOG_CREATE);
  vdb_table_definition(thd, form, dd, m.mutable_table());
  m.mutable_table()->set_database(db);
  m.mutable_table()->set_name(tname);
  return send_catalog(thd, m, &err_msg);
}

int ha_vdb_mysql_storage::delete_table(const char *name, const dd::Table *) {
  std::string db, tname;
  split_path(name, &db, &tname);
  if (is_tmp_name(tname)) return 0;
  vdb::Catalog m;
  m.set_kind(vdb::CATALOG_DROP);
  m.mutable_table()->set_database(db);
  m.mutable_table()->set_name(tname);
  return send_catalog(ha_thd(), m, &err_msg);
}

int ha_vdb_mysql_storage::rename_table(const char *from, const char *to, const dd::Table *, dd::Table *) {
  std::string db, tname, ndb, nname;
  split_path(from, &db, &tname);
  split_path(to, &ndb, &nname);
  if (is_tmp_name(tname) || is_tmp_name(nname)) return 0;
  vdb::Catalog m;
  m.set_kind(vdb::CATALOG_RENAME);
  m.mutable_table()->set_database(db);
  m.mutable_table()->set_name(tname);
  m.set_new_database(ndb);
  m.set_new_name(nname);
  return send_catalog(ha_thd(), m, &err_msg);
}

/* TRUNCATE and DELETE without WHERE both detach the table from the Source
   (product doc 05 §1; Source scaling plan 07, B1), with the same call. The
   engine keeps them apart only where MySQL does: a DELETE rolls back with
   its transaction and keeps AUTO_INCREMENT. */
int ha_vdb_mysql_storage::detach(uint8_t op, const google::protobuf::MessageLite &m,
                                 google::protobuf::MessageLite *reply) {
  if (int e = begin(ha_thd())) return e;
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return err;
  vdb::Error e;
  int rc = c->call(op, m, reply, &e);
  vdb_bump(vdb_st_requests);
  return rc == 0 ? 0 : fail(rc, e);
}

int ha_vdb_mysql_storage::truncate(dd::Table *) {
  vdb::Truncate m;
  m.set_table(tid);
  return detach(vdb_proto::OP_TRUNCATE, m, nullptr);
}

/* mysqld calls this only for a DELETE of every row; it reports the row count
   it read from info() just before as the affected rows. When the engine
   declines (foreign-key children to check or cascade), mysqld deletes row by
   row. */
int ha_vdb_mysql_storage::delete_all_rows() {
  vdb::DeleteAll m;
  m.set_table(tid);
  vdb::DeleteAllReply r;
  if (int e = detach(vdb_proto::OP_DELETE_ALL, m, &r)) return e;
  return r.declined() ? HA_ERR_WRONG_COMMAND : 0;
}

namespace {
using F = Alter_inplace_info;
const F::HA_ALTER_FLAGS kInstant = F::ALTER_COLUMN_DEFAULT | F::ADD_FOREIGN_KEY | F::DROP_FOREIGN_KEY |
                                   F::RENAME_INDEX | F::ALTER_INDEX_COMMENT | F::CHANGE_CREATE_OPTION |
                                   F::ADD_CHECK_CONSTRAINT | F::DROP_CHECK_CONSTRAINT | F::SUSPEND_CHECK_CONSTRAINT |
                                   F::ALTER_COLUMN_VISIBILITY;
/* Changes that keep the Source's columns but reshape the Delta's rows. */
const F::HA_ALTER_FLAGS kReshape =
    F::ADD_INDEX | F::DROP_INDEX | F::ADD_UNIQUE_INDEX | F::DROP_UNIQUE_INDEX | F::ADD_VIRTUAL_COLUMN |
    F::ADD_STORED_BASE_COLUMN | F::ADD_STORED_GENERATED_COLUMN | F::ADD_COLUMN | F::DROP_VIRTUAL_COLUMN |
    F::DROP_STORED_COLUMN | F::DROP_COLUMN | F::ALTER_VIRTUAL_COLUMN_ORDER | F::ALTER_STORED_COLUMN_ORDER |
    F::ALTER_COLUMN_EQUAL_PACK_LENGTH | F::ALTER_VIRTUAL_GCOL_EXPR | F::VIRTUAL_GCOL_REEVAL;
/* Changes a client may not make on a table read from the Source (D-45). */
const F::HA_ALTER_FLAGS kSourceOnly =
    F::ALTER_COLUMN_NAME | F::ALTER_STORED_COLUMN_TYPE | F::ALTER_VIRTUAL_COLUMN_TYPE | F::ADD_PK_INDEX |
    F::DROP_PK_INDEX | F::ALTER_COLUMN_NULLABLE | F::ALTER_COLUMN_NOT_NULLABLE | F::ALTER_COLUMN_INDEX_LENGTH |
    F::ALTER_STORED_GCOL_EXPR | F::STORED_GCOL_REEVAL | F::ALTER_COLUMN_STORAGE_TYPE | F::ALTER_COLUMN_COLUMN_FORMAT |
    F::CHANGE_INDEX_OPTION;

void kinds(F::HA_ALTER_FLAGS f, vdb::Alter *m) {
  if (f & (F::ADD_COLUMN | F::ADD_VIRTUAL_COLUMN | F::ADD_STORED_BASE_COLUMN | F::ADD_STORED_GENERATED_COLUMN))
    m->add_kinds(vdb::ALTER_ADD_COLUMN);
  if (f & (F::DROP_COLUMN | F::DROP_VIRTUAL_COLUMN | F::DROP_STORED_COLUMN)) m->add_kinds(vdb::ALTER_DROP_COLUMN);
  if (f & (F::ADD_INDEX | F::ADD_UNIQUE_INDEX)) m->add_kinds(vdb::ALTER_ADD_INDEX);
  if (f & (F::DROP_INDEX | F::DROP_UNIQUE_INDEX)) m->add_kinds(vdb::ALTER_DROP_INDEX);
  if (f & F::ALTER_COLUMN_DEFAULT) m->add_kinds(vdb::ALTER_CHANGE_DEFAULT);
  if (f & F::ADD_FOREIGN_KEY) m->add_kinds(vdb::ALTER_ADD_FOREIGN_KEY);
  if (f & F::DROP_FOREIGN_KEY) m->add_kinds(vdb::ALTER_DROP_FOREIGN_KEY);
  if (f & F::ALTER_COLUMN_NAME) m->add_kinds(vdb::ALTER_RENAME_COLUMN);
  if (f & (F::ALTER_STORED_COLUMN_TYPE | F::ALTER_VIRTUAL_COLUMN_TYPE | F::ALTER_COLUMN_NULLABLE |
           F::ALTER_COLUMN_NOT_NULLABLE | F::ALTER_COLUMN_EQUAL_PACK_LENGTH))
    m->add_kinds(vdb::ALTER_CHANGE_TYPE);
  if (f & (F::ADD_PK_INDEX | F::DROP_PK_INDEX)) m->add_kinds(vdb::ALTER_CHANGE_KEY);
  if (f & F::CHANGE_CREATE_OPTION) m->add_kinds(vdb::ALTER_OPTIONS);
}

/* The object an ALTER names: the first column or index added or dropped. */
std::string object_of(TABLE *old, TABLE *altered) {
  auto has_field = [](TABLE *t, const char *n) {
    for (uint i = 0; i < t->s->fields; i++)
      if (my_strcasecmp(system_charset_info, t->field[i]->field_name, n) == 0) return true;
    return false;
  };
  auto has_key = [](TABLE *t, const char *n) {
    for (uint i = 0; i < t->s->keys; i++)
      if (my_strcasecmp(system_charset_info, t->key_info[i].name, n) == 0) return true;
    return false;
  };
  for (uint i = 0; i < altered->s->fields; i++)
    if (!has_field(old, altered->field[i]->field_name)) return altered->field[i]->field_name;
  for (uint i = 0; i < old->s->fields; i++)
    if (!has_field(altered, old->field[i]->field_name)) return old->field[i]->field_name;
  for (uint i = 0; i < altered->s->keys; i++)
    if (!has_key(old, altered->key_info[i].name)) return altered->key_info[i].name;
  for (uint i = 0; i < old->s->keys; i++)
    if (!has_key(altered, old->key_info[i].name)) return old->key_info[i].name;
  return "";
}
}  // namespace

enum_alter_inplace_result ha_vdb_mysql_storage::check_if_supported_inplace_alter(TABLE *, Alter_inplace_info *ai) {
  F::HA_ALTER_FLAGS f = ai->handler_flags;
  if (f & ~(kInstant | kReshape | kSourceOnly)) return HA_ALTER_INPLACE_NOT_SUPPORTED;
  if (tid == 0 && begin(ha_thd()) != 0) return HA_ALTER_ERROR;
  if ((f & kSourceOnly) && mirrored && !vdb_is_control(ha_thd())) {
    err_msg =
        "VDB: renaming a column, changing its type or changing the primary key of a table read from the "
        "Source needs a table copy, which VDB refuses (D-45)";
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_VDB, err_msg.c_str(), "vdb_mysql_storage");
    return HA_ALTER_ERROR;
  }
  convert = (f & (kReshape | kSourceOnly)) != 0;
  return convert ? HA_ALTER_INPLACE_NO_LOCK : HA_ALTER_INPLACE_INSTANT;
}

/* The catalog change first, so the conversion's images use the new layout. */
bool ha_vdb_mysql_storage::prepare_inplace_alter_table(TABLE *altered, Alter_inplace_info *ai, const dd::Table *,
                                                       dd::Table *new_dd) {
  THD *thd = ha_thd();
  if (begin(thd) != 0) {
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_VDB, err_msg.c_str(), "vdb_mysql_storage");
    return true;
  }
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) {
    my_error(ER_GET_ERRMSG, MYF(0), err, err_msg.c_str(), "vdb_mysql_storage");
    return true;
  }
  vdb::Alter m;
  m.set_table(tid);
  kinds(ai->handler_flags, &m);
  m.set_object(object_of(table, altered));
  vdb_table_definition(thd, altered, new_dd, m.mutable_table_def());
  m.mutable_table_def()->set_database(table_share->db.str, table_share->db.length);
  m.mutable_table_def()->set_name(table_share->table_name.str, table_share->table_name.length);
  m.set_statement(thd->query().str, thd->query().length);
  vdb::AlterReply r;
  vdb::Error e;
  int rc = c->call(vdb_proto::OP_ALTER, m, &r, &e);
  if (rc != 0) {
    int code = fail(rc, e);
    my_error(ER_GET_ERRMSG, MYF(0), code, err_msg.c_str(), "vdb_mysql_storage");
    return true;
  }
  if (r.verdict() == vdb::ALTER_NEEDS_COPY) {
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_VDB, ("VDB: " + r.message()).c_str(), "vdb_mysql_storage");
    return true;
  }
  return false;
}

/* Convert every Delta row from the old layout to the new one with MySQL's
   own conversion rules (product doc 09 §5.1), and recompute its images. */
bool ha_vdb_mysql_storage::inplace_alter_table(TABLE *altered, Alter_inplace_info *ai, const dd::Table *, dd::Table *) {
  if (!convert) return false;
  int err = 0;
  Vdb_conn *c = conn(&err);
  if (c == nullptr) return true;
  vdb::AlterRows q;
  q.set_table(tid);
  vdb::ScanNextReply rows;
  vdb::Error e;
  if (c->call(vdb_proto::OP_ALTER_ROWS, q, &rows, &e) != 0) {
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_VDB, ("VDB: " + e.message()).c_str(), "vdb_mysql_storage");
    return true;
  }
  std::vector<uint32_t> saved = cols;
  cols.clear();
  for (uint i = 0; i < table->s->fields; i++) cols.push_back(i);
  /* For each new field: the old field it comes from, by Create_field. */
  std::vector<Field *> from(altered->s->fields, nullptr);
  uint i = 0;
  for (const Create_field &cf : ai->alter_info->create_list) {
    if (i < from.size()) from[i] = cf.field;
    i++;
  }
  vdb::AlterPut put;
  put.set_table(tid);
  my_bitmap_map *oldw = dbug_tmp_use_all_columns(altered, altered->write_set);
  bool failed = false;
  for (const vdb::Row &row : rows.batch().rows()) {
    decode_row(row, table->record[0]);
    memcpy(altered->record[0], altered->s->default_values, altered->s->reclength);
    vdb::Row *out = put.add_rows();
    for (uint k = 0; k < altered->s->fields; k++) {
      Field *nf = altered->field[k];
      if (from[k] != nullptr) {
        vdb::Value v;
        vdb_value_of(from[k], &v);
        vdb_store_value(nf, v);
      }
      vdb_value_of(nf, out->add_values());
    }
    vdb_images_of(altered, put.add_images());
    if (put.rows_size() >= 1000) {
      if (c->call(vdb_proto::OP_ALTER_PUT, put, nullptr, &e) != 0) failed = true;
      put.clear_rows();
      put.clear_images();
    }
  }
  if (!failed && put.rows_size() > 0 && c->call(vdb_proto::OP_ALTER_PUT, put, nullptr, &e) != 0) failed = true;
  dbug_tmp_restore_column_map(altered->write_set, oldw);
  cols = saved;
  if (failed) {
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_VDB, ("VDB: " + e.message()).c_str(), "vdb_mysql_storage");
    return true;
  }
  return false;
}

/* Commit and rollback reach vdb-core through the handlerton with the DDL's
   own transaction (atomic DDL). */
bool ha_vdb_mysql_storage::commit_inplace_alter_table(TABLE *, Alter_inplace_info *, bool, const dd::Table *,
                                                      dd::Table *) {
  convert = false;
  return false;
}
