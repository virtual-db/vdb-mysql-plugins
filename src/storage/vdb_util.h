/* vdb_util.h: free helpers shared by ddl.cc and images.cc. */
#ifndef VDB_STORAGE_UTIL_H
#define VDB_STORAGE_UTIL_H

#include <string>

#include "storage/vdb/common/proto.h"

class Field;
struct TABLE;
struct KEY_PART_INFO;
class THD;
namespace dd {
class Table;
}

void vdb_store_value(Field *f, const vdb::Value &v);
void vdb_value_of(Field *f, vdb::Value *v);
void vdb_images_of(TABLE *t, vdb::Images *out);
size_t vdb_part_len(const KEY_PART_INFO *kp);
/* The structured definition of a table, with mysqld's own CREATE TABLE text. */
void vdb_table_definition(THD *thd, TABLE *t, const dd::Table *dd, vdb::TableDefinition *out);

#endif
