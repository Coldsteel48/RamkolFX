#!/bin/sh
# Prints the CMake flags that make the FetchContent dependencies (libmysofa,
# KissFFT, Dear ImGui) resolve from already-unpacked source trees instead of
# the network. Used by the distro recipes, whose build sandboxes are offline.
#
# usage: vendor-flags.sh <dir containing libmysofa-X, kissfft-X, imgui-X>
set -eu
d="$1"
echo "-DFETCHCONTENT_FULLY_DISCONNECTED=ON" \
     "-DFETCHCONTENT_SOURCE_DIR_MYSOFA=$d/libmysofa-1.3.5" \
     "-DFETCHCONTENT_SOURCE_DIR_KISSFFT=$d/kissfft-131.2.0" \
     "-DFETCHCONTENT_SOURCE_DIR_IMGUI=$d/imgui-1.91.6"
