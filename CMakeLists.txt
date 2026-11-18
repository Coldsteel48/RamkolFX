# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).

cmake_minimum_required(VERSION 3.20)
project(RamkolFX VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

# Installed layout: bin/ramkolfxd, bin/ramkolfx-gui (if built),
# share/ramkolfx/hrtf/*, a systemd *user* unit for the daemon, docs/licenses.
option(RAMKOLFX_INSTALL_HRTF_DATA "Install the bundled HRTF .sofa files" ON)
set(RAMKOLFX_SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/user" CACHE PATH
    "Where to install the ramkolfxd systemd user unit")

add_subdirectory(common)
add_subdirectory(daemon)

option(RAMKOLFX_BUILD_TESTS "Build the unit tests (run with ctest)" ON)
if(RAMKOLFX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(RAMKOLFX_BUILD_GUI "Build the RamkolFX GUI control app" OFF)
if(RAMKOLFX_BUILD_GUI)
    add_subdirectory(gui)
endif()

if(RAMKOLFX_INSTALL_HRTF_DATA)
    install(DIRECTORY data/hrtf/
            DESTINATION ${CMAKE_INSTALL_DATADIR}/ramkolfx/hrtf
            COMPONENT ramkolfx)
endif()
configure_file(packaging/common/ramkolfxd.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/ramkolfxd.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ramkolfxd.service
        DESTINATION ${RAMKOLFX_SYSTEMD_USER_UNIT_DIR} COMPONENT ramkolfx)
install(FILES LICENSE LICENSE-COMMERCIAL.md README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT ramkolfx)
install(FILES data/hrtf/README.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/ramkolfx/hrtf COMPONENT ramkolfx)
