# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).

# Unit tests - run with `ctest --test-dir build` (or ./build.sh test).
# One executable per area; each uses the tiny header-only harness in
# test_harness.hpp, so the test build fetches nothing extra.

function(ramkolfx_add_test NAME)
    add_executable(${NAME} ${NAME}.cpp)
    target_link_libraries(${NAME} PRIVATE ${ARGN})
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

ramkolfx_add_test(protocol_tests ramkolfx_common)
ramkolfx_add_test(preset_store_tests ramkolfx_presets)
ramkolfx_add_test(preset_auto_switch_tests ramkolfx_presets)
ramkolfx_add_test(settings_store_tests ramkolfx_presets)
