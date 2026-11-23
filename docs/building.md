# Building

`build/build.sh` is the one build script, used by the `Dockerfile`, by `build/dev.sh` and by `vdb-mysql`'s
`Dockerfile`:

| Step | Does |
|---|---|
| `fetch [release]` | Downloads the release's source tarball named in `mysql-releases.txt` (the first line when none is given) and verifies its SHA-256 |
| `configure` | `cmake` with `build/cmake.options` (`-DWITH_DEBUG=1` when `VARIANT=debug`) |
| `server` | Builds `mysqld`, the client tools and the server plug-ins |
| `plugin` | Copies `src/` to `storage/vdb`, re-runs `cmake`, builds `vdb.so` only; `VDB_WERROR=ON` makes every warning in `src/` an error |
| `install` | Installs into `/usr/local/mysql`, removes test suites, test and example programs and plug-ins, headers and static libraries, and strips debug info from every binary (symbol tables stay, so backtraces still name functions) |

`build/toolchain.sh` installs the compilers and build dependencies on `oraclelinux:10-slim`: its own GCC, CMake,
`lld` and the libraries MySQL links. It is the one list of them; `vdb-mysql`'s `Dockerfile` runs it too.

`build/cmake.options` builds with `RelWithDebInfo`'s `-O2 -DNDEBUG` but without `-g`, links with `lld`, and keeps
MySQL's bundled ICU so `REGEXP` matches official MySQL builds. Router, NDB, the X plug-in, group replication and unit
tests are off.

`VARIANT=debug` (`docker build --build-arg VARIANT=debug --target installed .`) builds with MySQL's
assertions, maintainer warnings as errors, and the statistics poisoning the STATS suite reads. It is a local tool; no
workflow builds it and no image ships it.

`build/dev.sh` keeps the source tree and build directory in the Docker volume `vdb-mysql-work`, so a build that runs
out of memory on a small machine resumes where it stopped. `JOBS` sets the parallelism (default 2). After a change to
`build/toolchain.sh` or `build/cmake.options`, remove the volume (`docker volume rm vdb-mysql-work`) and run
`build/dev.sh server` again.

The protocol codec is generated at build time from `src/common/vdb.proto` with the `protoc` MySQL builds in its own
tree (`WITH_PROTOBUF=bundled`), and linked against its `libprotobuf-lite` (D-81).

## CI

`ci.yml` runs on pull requests: Lint (`clang-format` 23.1.5) and Test (`mysqld` from the `server` stage, cached in
`ghcr.io/virtual-db/vdb-mysql-plugins:buildcache`; `vdb.so` with `VDB_WERROR=ON`; a check that `mysqld` loads
`vdb.so` with four plug-ins ACTIVE). `mysql-releases.yml` checks weekly for a newer 8.4 patch release and opens a pull
request adding it.

### A new mysqld release needs proven collations first

vdb-mysql's image build fails for a mysqld release that has no entry in vdb-mysql-engine's
`internal/collations/proven.yaml` (vdb-mysql#23): without one, every non-binary string index would read whole
tables. Before a release added to `mysql-releases.txt` (by hand or by the weekly pull request) becomes the default
line or reaches an image, run the collation proof suite for it in vdb-tests (`runner collations`) and add its
result to `proven.yaml` by pull request to vdb-mysql-engine.
