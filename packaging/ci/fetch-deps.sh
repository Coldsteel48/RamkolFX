#!/bin/sh
# Downloads and unpacks the pinned FetchContent dependencies into <dir>
# (checksum-verified). Used by the Debian build, which has no declarative
# multi-source mechanism like the other recipes.
#
# usage: fetch-deps.sh <dir> [mysofa] [kissfft] [imgui]   (default: all three)
set -eu
d="$1"; shift; mkdir -p "$d"; cd "$d"
want="${*:-mysofa kissfft imgui}"
get() { # name url sha256
    case " $want " in *" $(echo "$1" | sed -E 's/^(lib)?([a-z]+)-.*/\2/') "*) ;; *) return 0 ;; esac
    [ -d "${1%.tar.gz}" ] && return 0
    curl -fsSL -o "$1" "$2"
    echo "$3  $1" | sha256sum -c -
    tar xzf "$1"
}
get libmysofa-1.3.5.tar.gz https://github.com/hoene/libmysofa/archive/refs/tags/v1.3.5.tar.gz f29508c335c83d8703f943ffc9ca783ac39aca84e851357f13a55af0f8143137
get kissfft-131.2.0.tar.gz https://github.com/mborgerding/kissfft/archive/refs/tags/131.2.0.tar.gz 205a8f6a448ef12b091f8ac6a514b5091bb5f6b0b543431ed75f673116cf5cbf
get imgui-1.91.6.tar.gz https://github.com/ocornut/imgui/archive/refs/tags/v1.91.6.tar.gz c5fbc5dcab1d46064001c3b84d7a88812985cde7e0e9ced03f5677bec1ba502a
