#!/bin/sh
# Runs inside a fedora container: build the .rpm; source tarball at /src/dist/RamkolFX-$VER.tar.gz.
set -eu
VER="$1"
dnf install -y rpm-build rpmdevtools dnf-plugins-core
cd /src
mkdir -p /root/rpmbuild/SOURCES
cp dist/RamkolFX-$VER.tar.gz /root/rpmbuild/SOURCES/
spectool -g -R packaging/rpm/ramkolfx.spec   # fetches Source1-2 (Source0 already present)
dnf builddep -y packaging/rpm/ramkolfx.spec
rpmbuild -bb packaging/rpm/ramkolfx.spec
cp /root/rpmbuild/RPMS/*/ramkolfx-*.rpm dist/
