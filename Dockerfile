# vdb-mysql-plugins: Oracle's mysqld 8.4 built from source with vdb.so (implementation plan doc 14 §2.2).
#
# Stages:
#   toolchain  compilers and build dependencies
#   server     the configured source tree and build directory with mysqld built (no vdb.so yet)
#   builder    server + vdb.so; kept so a plug-in change rebuilds only vdb.so (`build.sh plugin`)
#   dist       the installed /usr/local/mysql prefix with vdb.so, COPYING and the source offer
# Build the debug variant with --build-arg VARIANT=debug.

ARG BASE=oraclelinux:9-slim

FROM ${BASE} AS toolchain
RUN microdnf install -y --enablerepo=ol9_codeready_builder \
      gcc-toolset-12-gcc gcc-toolset-12-gcc-c++ gcc-toolset-12-binutils \
      gcc-toolset-12-annobin-annocheck gcc-toolset-12-annobin-plugin-gcc \
      make cmake bison perl tar gzip xz findutils diffutils which \
      openssl-devel ncurses-devel libaio-devel libtirpc-devel rpcgen pkgconf-pkg-config procps-ng \
    && microdnf clean all
ENV PATH=/opt/rh/gcc-toolset-12/root/usr/bin:$PATH \
    CC=/opt/rh/gcc-toolset-12/root/usr/bin/gcc \
    CXX=/opt/rh/gcc-toolset-12/root/usr/bin/g++

FROM toolchain AS server
ARG MYSQL_RELEASE=
ARG VARIANT=release
ARG JOBS=4
ENV SRC=/work/mysql BLD=/work/build VDB_SRC=/vdb/src VARIANT=${VARIANT} JOBS=${JOBS}
COPY build/ /vdb/build/
RUN --mount=type=cache,target=/cache /vdb/build/build.sh fetch "${MYSQL_RELEASE}"
RUN /vdb/build/build.sh configure
RUN /vdb/build/build.sh server

FROM server AS builder
COPY src/ /vdb/src/
RUN /vdb/build/build.sh plugin

FROM builder AS installed
RUN DESTDIR=/out /vdb/build/build.sh install

FROM scratch AS dist
COPY --from=installed /out/usr/local/mysql /usr/local/mysql
COPY COPYING /usr/local/mysql/share/vdb/COPYING
COPY docs/source-offer.md /usr/local/mysql/share/vdb/source-offer.md
