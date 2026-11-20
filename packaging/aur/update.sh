#!/bin/sh
# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).
#
# Writes the AUR package (PKGBUILD + .SRCINFO) for the version in ./VERSION
# into a checkout of the AUR repo. packaging/arch/PKGBUILD carries 'SKIP' for
# the release tarball, because version.sh syncs it before the tag exists; the
# AUR copy gets the real checksum of GitHub's archive of the tag.
#
# usage: packaging/aur/update.sh <aur-checkout-dir>
#   e.g. git clone ssh://aur@aur.archlinux.org/ramkolfx.git /tmp/aur-ramkolfx
#        packaging/aur/update.sh /tmp/aur-ramkolfx
# Run as a normal user (makepkg refuses root). Needs curl and makepkg.
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:?usage: $0 <aur-checkout-dir>}
[ -d "$out" ] || { echo "error: $out is not a directory" >&2; exit 1; }

V=$(tr -d '[:space:]' < "$repo/VERSION")
url="https://github.com/Coldsteel48/RamkolFX/archive/v$V.tar.gz"
sum=$(curl -fsSL "$url" | sha256sum | cut -d' ' -f1)
[ -n "$sum" ] || { echo "error: could not download $url" >&2; exit 1; }

sed -E "s|^sha256sums=\('SKIP'.*|sha256sums=('$sum'|" \
    "$repo/packaging/arch/PKGBUILD" > "$out/PKGBUILD"
if grep -q "'SKIP'" "$out/PKGBUILD"; then
    echo "error: PKGBUILD still has a SKIP checksum" >&2
    exit 1
fi
(cd "$out" && makepkg --printsrcinfo > .SRCINFO)
echo "AUR package for $V written to $out (tarball sha256 $sum)"
