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
ramkolfx_add_test(content_detection_tests ramkolfx_presets)
ramkolfx_add_test(game_detection_tests ramkolfx_presets)
ramkolfx_add_test(content_detector_tests ramkolfx_detect)
# Needs ONNX Runtime at run time (dlopened); exits 77 = skipped without it.
ramkolfx_add_test(onnx_classifier_tests ramkolfx_detect)
set_tests_properties(onnx_classifier_tests PROPERTIES SKIP_RETURN_CODE 77)
# Finding the runtime's library file; runs without ONNX Runtime.
ramkolfx_add_test(ort_library_lookup_tests ramkolfx_detect)

# The GUI's autostart Exec= quoting. Pure string code with no GUI
# dependencies, compiled straight in so it's tested even when the GUI
# itself isn't built (RAMKOLFX_BUILD_GUI is off by default).
ramkolfx_add_test(desktop_exec_tests)
target_sources(desktop_exec_tests PRIVATE ${PROJECT_SOURCE_DIR}/gui/src/desktop_exec.cpp)
target_include_directories(desktop_exec_tests PRIVATE ${PROJECT_SOURCE_DIR}/gui/src)

# End-to-end client against a live daemon + PipeWire - not a ctest test and
# not built by default; tests/e2e/run.sh builds and drives it.
add_executable(ramkolfx_e2e EXCLUDE_FROM_ALL e2e/e2e_client.cpp)
target_link_libraries(ramkolfx_e2e PRIVATE ramkolfx_common)
