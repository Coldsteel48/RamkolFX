#!/bin/sh
# Runs inside an archlinux container: build the pacman package; source tarball at /src/dist/RamkolFX-$VER.tar.gz.
set -eu
VER="$1"
pacman -Syu --noconfirm base-devel cmake ninja pipewire libmysofa kissfft appstream desktop-file-utils sdl2 fontconfig zlib libgl dbus libx11 sudo onnxruntime-cpu
useradd -m builder
rm -rf /work && mkdir /work
cp /src/packaging/arch/PKGBUILD /work/ && cp /src/dist/RamkolFX-$VER.tar.gz /work/
chown -R builder /work
su builder -c 'cd /work && makepkg --noconfirm --skipchecksums'
cp /work/ramkolfx-*.pkg.tar.zst /src/dist/
