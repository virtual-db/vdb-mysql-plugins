/* lockwait.cc: when vdb-core makes a session wait on a row lock, report the
   wait to mysqld with thd_report_row_lock_wait, as InnoDB does (product doc
   03 §2.9), so mysqld sees waits that cross its own metadata locks. */
#include "mysql/service_thd_engine_lock.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/sql_class.h"

void vdb_report_wait(THD *thd, uint64_t holder) {
  if (thd == nullptr || holder == 0) return;
  Find_thd_with_id find(static_cast<my_thread_id>(holder));
  THD_ptr other = Global_THD_manager::get_instance()->find_thd(&find);
  if (other) thd_report_row_lock_wait(thd, other.get());
}
