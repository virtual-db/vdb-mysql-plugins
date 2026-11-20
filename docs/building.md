# Building

`build/build.sh` is the one build script, used by the `Dockerfile` and by `build/dev.sh`:

| Step | Does |
|---|---|
| `fetch [release]` | Downloads the release's source tarball named in `mysql-releases.txt` and verifies its SHA-256 |
| `configure` | `cmake` with `build/cmake.options` (`-DWITH_DEBUG=1` when `VARIANT=debug`) |
| `server` | Builds `mysqld`, the client tools and the server plug-ins |
| `plugin` | Copies `src/` to `storage/vdb`, re-runs `cmake`, builds `vdb.so` only |
| `install` | Installs into `/usr/local/mysql` and removes test suites, headers and static libraries |

`build/dev.sh` keeps the source tree and build directory in the Docker volume `vdb-mysql-work`, so a build that runs
out of memory on a small machine resumes where it stopped. `JOBS` sets the parallelism (default 2).

The protocol codec is generated at build time from `src/common/vdb.proto` with the `protoc` MySQL builds in its own
tree (`WITH_PROTOBUF=bundled`), and linked against its `libprotobuf-lite` (D-81).
