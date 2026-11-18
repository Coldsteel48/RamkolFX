#!/bin/sh
# Runs inside an alpine container: build the .apk; source tarball at /src/dist/RamkolFX-$VER.tar.gz.
set -eu
VER="$1"
apk update && apk add --no-cache alpine-sdk sudo
adduser -D builder && addgroup builder abuild
echo 'builder ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/builder
rm -rf /work && mkdir -p /work/dist
cp -r /src/packaging/alpine/APKBUILD /work/
mkdir -p /var/cache/distfiles && cp /src/dist/RamkolFX-$VER.tar.gz /var/cache/distfiles/
# abuild builds with the full tree available under $srcdir/RamkolFX-$VER via the tarball itself.
chown -R builder /work /var/cache/distfiles
su builder -c 'cd /work && abuild-keygen -a -i -n && abuild checksum && abuild -r'
find /home/builder/packages -name 'ramkolfx-*.apk' -exec cp {} /src/dist/ \;
cp "$(ls /home/builder/.abuild/*.rsa.pub | head -1)" /src/dist/ramkolfx-alpine-key.rsa.pub
ls -l /src/dist
