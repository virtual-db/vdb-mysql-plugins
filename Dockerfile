# vdb-mysql-plugins: Oracle's mysqld 8.4 built from source with vdb.so (implementation plan doc 14 §2.2).
#
# This repository publishes no image (D-87): vdb-mysql builds the environment
# image from this source. These stages serve CI and local work.
#
# Stages:
#   toolchain  compilers and build dependencies
#   server     the configured source tree and build directory with mysqld built (no vdb.so yet);
#              cached, so a plug-in change rebuilds only vdb.so
#   installed  server + vdb.so, installed into /out/usr/local/mysql with COPYING and the source offer
# Build the debug variant with --build-arg VARIANT=debug.

ARG BASE=oraclelinux:10-slim

FROM ${BASE} AS toolchain
COPY build/toolchain.sh /tmp/toolchain.sh
RUN /tmp/toolchain.sh

FROM toolchain AS server
ARG MYSQL_RELEASE=
ARG VARIANT=release
ARG JOBS=4
ENV SRC=/work/mysql BLD=/work/build VDB_SRC=/vdb/src VARIANT=${VARIANT} JOBS=${JOBS}
COPY build/ /vdb/build/
RUN --mount=type=cache,target=/cache /vdb/build/build.sh fetch "${MYSQL_RELEASE}"
RUN /vdb/build/build.sh configure
RUN /vdb/build/build.sh server

FROM server AS installed
ARG MYSQL_RELEASE=
# VDB_WERROR=ON (CI) fails the build on any warning in vdb.so's own sources.
ARG VDB_WERROR=OFF
ENV VDB_WERROR=${VDB_WERROR}
COPY src/ /vdb/src/
RUN /vdb/build/build.sh plugin \
    && DESTDIR=/out /vdb/build/build.sh install \
    && mkdir -p /out/usr/local/mysql/share/vdb \
    && cat /work/mysql/.vdb-release > /out/usr/local/mysql/share/vdb/release
COPY COPYING /out/usr/local/mysql/share/vdb/COPYING
COPY docs/source-offer.md /out/usr/local/mysql/share/vdb/source-offer.md
