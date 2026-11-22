#!/bin/sh
# toolchain.sh: installs the compilers and MySQL's build dependencies on the
# Oracle Linux 10 base. The one list of them: this repository's Dockerfile and
# vdb-mysql's both run it. Oracle Linux 10's own GCC (14) builds mysqld 8.4.
set -e
pkgs="gcc gcc-c++ binutils make cmake bison perl tar gzip xz findutils diffutils which
  openssl-devel ncurses-devel libaio-devel libtirpc-devel rpcgen pkgconf-pkg-config procps-ng"
# Boost.Multiprecision (MySQL's GIS code) includes quadmath.h on x86_64, where
# GCC ships it separately; aarch64 has no libquadmath.
[ "$(uname -m)" = x86_64 ] && pkgs="$pkgs libquadmath-devel"
# shellcheck disable=SC2086
microdnf install -y --enablerepo=ol10_codeready_builder --setopt=install_weak_deps=0 $pkgs
microdnf clean all
