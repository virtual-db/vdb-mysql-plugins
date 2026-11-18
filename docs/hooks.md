# Hooks

Which file implements each MySQL hook (implementation plan doc 08 §3 to §5).

| File | Hooks |
|---|---|
| `storage/ha_vdb.cc` | `open`, `close`, `table_flags`, `index_flags`, `extra`, `reset`, `external_lock`, `start_stmt`, `store_lock`, `get_error_message` |
| `storage/hton.cc` | `commit`, `rollback`, `savepoint_set`, `savepoint_rollback`, `savepoint_release`, `start_consistent_snapshot`, `close_connection`, `kill_connection`, `post_ddl` |
| `storage/scan.cc` | `rnd_init`, `rnd_next`, `rnd_end`, `position`, `rnd_pos`, `index_init`, `index_end`, `index_read_map`, `index_read_last_map`, `index_first`, `index_last`, `index_next`, `index_prev`, `index_next_same`, `read_range_first` |
| `storage/interleave.cc` | The ordered merge of the Source and Delta cursors |
| `storage/mrr.cc` | `multi_range_read_info_const`, `multi_range_read_info`, `multi_range_read_init`, `multi_range_read_next` |
| `storage/rows.cc` | Value conversion with `Field::val_str` and `Field::store`; TIMESTAMP as Unix time |
| `storage/keys.cc` | Key formats and sort images with `Field::make_sort_key` |
| `storage/cond.cc` | `cond_push`, `idx_cond_push`, `push_to_engine` |
| `storage/write.cc` | `write_row`, `update_row`, `delete_row`, `start_bulk_insert`, `end_bulk_insert`, `get_auto_increment` |
| `storage/ddl.cc` | `create`, `delete_table`, `rename_table`, `truncate`, `delete_all_rows`, `check_if_supported_inplace_alter`, `prepare_inplace_alter_table`, `inplace_alter_table`, `commit_inplace_alter_table` |
| `storage/stats.cc` | `info`, `records`, `records_in_range`, `scan_time`, `read_time` |
| `storage/lockwait.cc` | `thd_report_row_lock_wait` on a `Waiting` frame |
| `storage/images.cc` | Sort images for cascaded rows (`ImagesNeeded`, D-86) |
| `auth/relay.cc`, `auth/caching_sha2.cc`, `auth/native.cc` | `st_mysql_auth.authenticate_user` for both login plug-ins |
| `statements/*.cc` | `MYSQL_AUDIT_PARSE_PREPARSE`, `MYSQL_AUDIT_QUERY_START`, `MYSQL_AUDIT_QUERY_STATUS_END`, connection events |
