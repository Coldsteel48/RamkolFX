#!/bin/sh
# Runs inside a debian container: build the .deb from the checkout at /src.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential debhelper devscripts fakeroot
cd /src
rm -rf debian && cp -r packaging/debian debian
apt-get install -y --no-install-recommends cmake ninja-build pkg-config libpipewire-0.3-dev zlib1g-dev libsdl2-dev libgl-dev libfontconfig-dev curl ca-certificates
dpkg-buildpackage -b -uc -us
mkdir -p dist && mv ../ramkolfx_*.deb dist/
rm -rf debian
