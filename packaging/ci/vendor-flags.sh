#!/bin/sh
# Prints the CMake flags that make the FetchContent dependencies (libmysofa,
# KissFFT, Dear ImGui) resolve from already-unpacked source trees instead of
# the network. Used by the distro recipes, whose build sandboxes are offline.
#
# usage: vendor-flags.sh <dir containing libmysofa-X, kissfft-X and/or imgui-X>
set -eu
# Only the trees present are passed, so a recipe that links the system
# libmysofa / KissFFT (-DRAMKOLFX_SYSTEM_*=ON) unpacks just Dear ImGui.
d="$1"
flags="-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
[ -d "$d/libmysofa-1.3.5" ] && flags="$flags -DFETCHCONTENT_SOURCE_DIR_MYSOFA=$d/libmysofa-1.3.5"
[ -d "$d/kissfft-131.2.0" ] && flags="$flags -DFETCHCONTENT_SOURCE_DIR_KISSFFT=$d/kissfft-131.2.0"
[ -d "$d/imgui-1.91.6" ] && flags="$flags -DFETCHCONTENT_SOURCE_DIR_IMGUI=$d/imgui-1.91.6"
echo "$flags"
