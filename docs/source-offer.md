# Source offer

This image contains `mysqld` built from Oracle's MySQL Community Server source, and `vdb.so`, both under the GNU
General Public License version 2 (`COPYING`).

| Component | Source |
|---|---|
| `mysqld` | The release named in the file `release` next to this one, from `https://cdn.mysql.com/Downloads/MySQL-8.4/mysql-<release>.tar.gz`. Its SHA-256 is on that release's line of `build/mysql-releases.txt`, and it is built with the `build/` directory, of the `vdb-mysql-plugins` tag named in the image's `net.vdbm.plugins.ref` label. |
| `vdb.so` | `src/` of the same `vdb-mysql-plugins` tag, at `https://github.com/virtual-db/vdb-mysql-plugins` |

On request, for three years from the date you received this image, we provide the complete corresponding source
code on a medium customarily used for software interchange, for no more than the cost of physically performing the
distribution. Write to admin@anqor.net.
