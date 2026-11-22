# vdb-mysql-plugins

The C++ half of VDB for MySQL: `vdb.so`, four plug-ins loaded into an unmodified, MySQL-compatible `mysqld` 8.4,
and the scripts that build Oracle's `mysqld` from source so the plug-ins match it exactly.

| Plug-in | Type | Does |
|---|---|---|
| `vdb_mysql_storage` | Storage engine | Every table: reads, writes, transactions, DDL forwarded to `vdb-mysql` |
| `vdb_mysql_auth` | Authentication | Log in with Source credentials, `caching_sha2_password` clients |
| `vdb_mysql_auth_native` | Authentication | The same for `mysql_native_password` clients |
| `vdb_mysql_statements` | Audit | Removes `ENGINE=` from `CREATE TABLE`, records view, routine and trigger DDL, emits statement events |

Every decision lives in Go (`vdb-mysql-engine`, `vdb-core`): the C++ calls MySQL's own functions, moves bytes over the plug-in
protocol, and applies the answers. Each file in `src/` says what it implements and which part of the plan it follows.

## Build

```sh
build/dev.sh server   # once: fetch, configure and build mysqld (resumable)
build/dev.sh plugin   # after a change in src/: rebuild vdb.so only
build/dev.sh dist     # wrap the installed prefix as the image vdb-mysql-plugins:dev (used by vdb-tests)
```

`mysqld` is MySQL 8.4 from Oracle's source (the first line of `build/mysql-releases.txt`), built on Oracle Linux 10
with its own GCC. This repository publishes no image: `vdb-mysql` builds the environment image from this source at a
tag in its `PLUGINS_REF`, and publishes it when a GitHub release is published and the oracle passes (D-91). The
`Dockerfile` here (`toolchain`, `server`, `installed`) serves CI and local work. See `docs/building.md`.

## Licence

GPLv2 (`COPYING`), because `vdb.so` is built against MySQL's server headers. The source offer for the shipped
`mysqld` is in `docs/source-offer.md`. VDB is MySQL-compatible; MySQL is a trademark of Oracle.
