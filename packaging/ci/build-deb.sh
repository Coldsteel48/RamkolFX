#!/bin/sh
# Runs inside a debian container: build the .deb from the checkout at /src.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential debhelper devscripts fakeroot
cd /src
rm -rf debian && cp -r packaging/debian debian
apt-get build-dep -y ./   # Build-Depends from debian/control
dpkg-buildpackage -b -uc -us
mkdir -p dist && mv ../ramkolfx_*.deb dist/
rm -rf debian
