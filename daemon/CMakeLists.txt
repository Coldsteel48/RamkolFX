# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
find_package(Threads REQUIRED)

# libmysofa (HRTF/SOFA file loading) and KissFFT (partitioned convolution)
# back BinauralStage's HRTF binaural decode ("Advanced" spatial mode). Both
# are BSD-3-Clause (permissive), matching the constraint in README.md's
# "Engineering approach" section.
#
# By default both are built from pinned sources (FetchContent) and linked
# statically, so a plain `cmake` works anywhere. Distro packages use the
# system copies instead, as their policies require: pass
# -DRAMKOLFX_SYSTEM_MYSOFA=ON / -DRAMKOLFX_SYSTEM_KISSFFT=ON (pkg-config
# modules libmysofa and kissfft-float). Explicit options rather than
# auto-detection, so what a package links never depends on what happened
# to be installed on the build machine.
option(RAMKOLFX_SYSTEM_MYSOFA "Link the system libmysofa instead of building the pinned copy" OFF)
option(RAMKOLFX_SYSTEM_KISSFFT "Link the system KissFFT (float) instead of building the pinned copy" OFF)
include(FetchContent)

add_library(ramkolfx_mysofa INTERFACE)
if(RAMKOLFX_SYSTEM_MYSOFA)
    pkg_check_modules(MYSOFA REQUIRED IMPORTED_TARGET libmysofa>=1.3)
    target_link_libraries(ramkolfx_mysofa INTERFACE PkgConfig::MYSOFA)
else()
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        mysofa
        GIT_REPOSITORY https://github.com/hoene/libmysofa.git
        GIT_TAG v1.3.5
    )
    # Linked statically into ramkolfxd. Its own install rules (headers,
    # cmake configs) must not end up in packages: RamkolFX's files all use
    # COMPONENT ramkolfx, so install with `cmake --install <dir> --component ramkolfx`.
    FetchContent_MakeAvailable(mysofa)
    target_link_libraries(ramkolfx_mysofa INTERFACE mysofa-static)
    # The generated mysofa_export.h, not part of mysofa-static's public interface.
    target_include_directories(ramkolfx_mysofa INTERFACE ${mysofa_BINARY_DIR}/src)
endif()

add_library(ramkolfx_kissfft INTERFACE)
if(RAMKOLFX_SYSTEM_KISSFFT)
    # The .pc adds -I<includedir>/kissfft, so <kiss_fftr.h> resolves as with
    # the pinned copy. Any 131.x has the kiss_fftr API used here.
    pkg_check_modules(KISSFFT REQUIRED IMPORTED_TARGET kissfft-float>=131)
    target_link_libraries(ramkolfx_kissfft INTERFACE PkgConfig::KISSFFT)
else()
    # Distro builds may pass -DBUILD_SHARED_LIBS=ON (Fedora's %cmake does),
    # which KissFFT refuses next to KISSFFT_STATIC. A normal variable
    # shadows that cache entry here and in KissFFT's directory only.
    set(BUILD_SHARED_LIBS OFF)
    set(KISSFFT_STATIC ON CACHE BOOL "" FORCE)
    set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
    set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
    set(KISSFFT_PKGCONFIG OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        kissfft
        GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
        GIT_TAG 131.2.0
    )
    FetchContent_MakeAvailable(kissfft)
    target_link_libraries(ramkolfx_kissfft INTERFACE kissfft::kissfft)
endif()

# Full switchable HRTF catalog (see data/hrtf/README.md): the bundled MIT
# KEMAR default, the bundled SADIE II / HUTUBS / SONICOM subjects
# (Apache-2.0 / CC BY 4.0 / MIT respectively, all safe to redistribute -
# unlike the CIPIC/ARI/Listen/Aachen mirrors this project deliberately
# avoids bundling), and the license-free synthetic spherical-head model.
# AudioEngine filters this list down to entries that are actually present
# on disk at startup.
# Where the .sofa files live at *runtime*. Defaults to the source tree (dev
# builds); distro packages pass -DRAMKOLFX_HRTF_DATA_DIR=/usr/share/ramkolfx/hrtf
# so the baked-in catalog paths match the installed layout.
set(RAMKOLFX_HRTF_DATA_DIR "${CMAKE_SOURCE_DIR}/data/hrtf" CACHE PATH
    "Runtime directory holding the bundled HRTF .sofa files")

set(RAMKOLFX_HRTF_CATALOG_ENTRIES
    "    { \"MIT KEMAR (normal pinna)\", HrtfSourceKind::SofaFile, \"${RAMKOLFX_HRTF_DATA_DIR}/default.sofa\" },\n")
set(RAMKOLFX_SADIE_SUBJECTS D1 D2 H3 H4 H5 H6 H7 H8 H9 H10 H11 H12 H13 H14 H15 H16 H17 H18 H19 H20)
foreach(SUBJECT ${RAMKOLFX_SADIE_SUBJECTS})
    string(APPEND RAMKOLFX_HRTF_CATALOG_ENTRIES
        "    { \"SADIE II - ${SUBJECT}\", HrtfSourceKind::SofaFile, \"${RAMKOLFX_HRTF_DATA_DIR}/sadie/sadie_${SUBJECT}.sofa\" },\n")
endforeach()
set(RAMKOLFX_HUTUBS_SUBJECTS pp5 pp30 pp50 pp70 pp90)
foreach(SUBJECT ${RAMKOLFX_HUTUBS_SUBJECTS})
    string(APPEND RAMKOLFX_HRTF_CATALOG_ENTRIES
        "    { \"HUTUBS - ${SUBJECT}\", HrtfSourceKind::SofaFile, \"${RAMKOLFX_HRTF_DATA_DIR}/hutubs/hutubs_${SUBJECT}.sofa\" },\n")
endforeach()
set(RAMKOLFX_SONICOM_SUBJECTS P0002 P0057 P0102 P0201 P0301)
foreach(SUBJECT ${RAMKOLFX_SONICOM_SUBJECTS})
    string(APPEND RAMKOLFX_HRTF_CATALOG_ENTRIES
        "    { \"SONICOM - ${SUBJECT}\", HrtfSourceKind::SofaFile, \"${RAMKOLFX_HRTF_DATA_DIR}/sonicom/sonicom_${SUBJECT}.sofa\" },\n")
endforeach()
string(APPEND RAMKOLFX_HRTF_CATALOG_ENTRIES
    "    { \"Synthetic spherical head\", HrtfSourceKind::SyntheticSphericalHead, \"\" },\n")

configure_file(
    src/hrtf_catalog.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/hrtf_catalog.hpp
    @ONLY
)

# EQ preset storage + auto-switching logic (including the pure side of
# Auto-detect: label folding, smoothing, inference pacing), plus the EQ defaults they
# depend on: no PipeWire, no DSP pipeline, so the tests (tests/) can link
# exactly the same objects the daemon runs.
add_library(ramkolfx_presets STATIC
    src/preset_store.cpp
    src/preset_auto_switch.cpp
    src/content_detection.cpp
    src/settings_store.cpp
    src/dsp/biquad.cpp
    src/dsp/graphic_eq_filter.cpp
)
target_include_directories(ramkolfx_presets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ramkolfx_presets PUBLIC ramkolfx_common)

# Auto-detect's inference side (docs/content-detect-plan.md): the worker
# thread and the ONNX Runtime classifier. ORT is dlopened at runtime, not
# linked - only its (vendored, MIT) C API header is needed to build - so
# builds and CI need no ORT and the daemon runs without it.
# Distro packages pass -DRAMKOLFX_MODEL_DATA_DIR=/usr/share/ramkolfx/models
# (like RAMKOLFX_HRTF_DATA_DIR); RAMKOLFX_MODEL_DIR in the environment
# overrides it at run time.
set(RAMKOLFX_MODEL_DATA_DIR "${CMAKE_SOURCE_DIR}/data/models" CACHE PATH
    "Runtime directory holding the Auto-detect classifier model")
add_library(ramkolfx_detect STATIC
    src/detect/content_detector.cpp
    src/detect/onnx_classifier.cpp
    src/detect/wav_file.cpp # bench + tests only; never linked into ramkolfxd
)
target_include_directories(ramkolfx_detect SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime)
# Normally empty: the daemon searches for libonnxruntime itself. Set it
# where the dynamic linker can't find the library by name (Nix).
set(RAMKOLFX_ORT_LIBRARY "" CACHE STRING
    "Full path of the ONNX Runtime library to load first (empty: search)")
target_compile_definitions(ramkolfx_detect PRIVATE RAMKOLFX_MODEL_DIR="${RAMKOLFX_MODEL_DATA_DIR}"
                           RAMKOLFX_ORT_LIBRARY="${RAMKOLFX_ORT_LIBRARY}")
target_link_libraries(ramkolfx_detect PUBLIC ramkolfx_presets Threads::Threads ${CMAKE_DL_LIBS})

add_executable(ramkolfxd
    src/main.cpp
    src/audio_engine.cpp
    src/virtual_sink.cpp
    src/hardware_output.cpp
    src/device_registry.cpp
    src/single_instance_lock.cpp
    src/dsp/passthrough_stage.cpp
    src/dsp/speaker_layout.cpp
    src/dsp/ambisonics_stage.cpp
    src/dsp/hrtf_loader.cpp
    src/dsp/synthetic_hrtf.cpp
    src/dsp/near_field_filter.cpp
    src/dsp/hrtf_gain_normalization.cpp
    src/dsp/partitioned_convolver.cpp
    src/dsp/binaural_stage.cpp
    src/dsp/binaural_voice.cpp
    src/dsp/hrtf_deck.cpp
    src/dsp/hw_eq_stage.cpp
    src/dsp/bass_enhancer_filter.cpp
    src/dsp/bass_enhancer_stage.cpp
    src/control/control_server.cpp
)

target_include_directories(ramkolfxd PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/src # so the generated headers above can #include "dsp/..." like everything else does
)

target_link_libraries(ramkolfxd PRIVATE
    ramkolfx_common
    ramkolfx_presets
    ramkolfx_detect
    PkgConfig::PIPEWIRE
    Threads::Threads
    ramkolfx_mysofa
    ramkolfx_kissfft
)

install(TARGETS ramkolfxd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ramkolfx)
configure_file(${PROJECT_SOURCE_DIR}/packaging/common/ramkolfxd.1.in
               ${CMAKE_CURRENT_BINARY_DIR}/ramkolfxd.1 @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ramkolfxd.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 COMPONENT ramkolfx)

# Measures Auto-detect against its performance budget on this machine
# (docs/content-detect-plan.md). Built so it can't rot, never installed.
add_executable(ramkolfx-detect-bench bench/detect_bench.cpp)
target_link_libraries(ramkolfx-detect-bench PRIVATE ramkolfx_detect)
