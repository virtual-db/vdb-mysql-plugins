# Contributing to vdb-mysql-plugins

> [!IMPORTANT]
> ### Public Contributions Are Not Yet Open
>
> VirtualDB is not currently accepting public pull requests. Please open an issue instead.

## What belongs here

Only the C++ plug-ins (`src/`) and the scripts that build `mysqld` with them (`build/`, `Dockerfile`). Logic that
decides anything (visibility, pushdown, logins, privileges, which clause to remove) belongs in `vdb-mysql-engine` or
`vdb-core`; the C++ calls MySQL's functions and forwards results.

## Rules

- One file per concern, with a header comment saying what it does and which plan section it implements.
- `.clang-format` (Google, as MySQL's tree; checked with clang-format 23.1.5: `pip install clang-format==23.1.5`); no exceptions; errors are `HA_ERR_*` and `CR_*` codes.
- No C++ unit tests: behaviour is tested from Go in `vdb-tests` (the ENGINE, STATS, ALTER, STATEMENTS, PROTO and FAIL
  suites).
- `src/common/vdb.proto` is a copy of `vdb-mysql-engine/protocol/vdb.proto`; change it there and copy it here.
- CI builds `vdb.so` with `VDB_WERROR=ON`: any compiler warning in `src/` fails the Test check. MySQL's own warnings
  do not.
