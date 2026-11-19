# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).

add_library(ramkolfx_common STATIC
    src/atomic_file.cpp
    src/key_value_file.cpp
    src/protocol.cpp
)

configure_file(
    include/ramkolfx/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ramkolfx/version.hpp
    @ONLY
)

target_include_directories(ramkolfx_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_compile_features(ramkolfx_common PUBLIC cxx_std_20)
