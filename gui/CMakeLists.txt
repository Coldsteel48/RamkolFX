# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).

# Dear ImGui + SDL2/OpenGL3 control app. Talks to ramkolfxd purely over
# the Unix control socket (see common/include/ramkolfx/protocol.hpp) -
# deliberately no PipeWire dependency here.
#
# Dear ImGui (MIT-licensed) is vendored via FetchContent rather than a
# system package: it avoids Qt/GTK's LGPL relinking obligations mattering
# for RamkolFX's commercial license track, and sidesteps cross-distro
# Qt5/Qt6 or GTK version skew since ImGui is meant to be compiled straight
# into the app rather than linked as a versioned shared library.

find_package(PkgConfig REQUIRED)
# 2.0.18: SDL_GetTicks64 - see main.cpp. Checked here so an older SDL fails
# at configure time with a clear message rather than mid-build. (Before
# 2.0.22, SDL also queues an SDL_QUIT when the last window is closed;
# main.cpp tells that apart from a real quit request, so close-to-tray
# works there too.)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2>=2.0.18)
find_package(OpenGL REQUIRED)

# Used at startup to look up already-installed system fonts per script
# (CJK, Thai, Cyrillic, ...) so the UI can render non-Latin text using
# whatever font the user's own desktop/locale already provides, instead
# of bundling a font file and taking on its license terms - see main.cpp.
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

# System tray icon (freedesktop StatusNotifierItem) and single-instance
# activation - see tray.hpp. libdbus is dual AFL-2.1/GPL-2.0+, and SDL
# already talks to the session bus through it, so this adds no new
# runtime dependency in practice.
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)

# FdWakeup's helper thread (fd_wakeup.hpp), and dlopen() for reusing SDL's
# already-loaded libX11 (x11_window.hpp).
find_package(Threads REQUIRED)

# x11_window.cpp needs Xlib's headers (only the headers - libX11 itself is
# reused from SDL at runtime). Without them it still builds, as a no-op,
# but minimize-to-tray then can't take a minimized window off the X11
# taskbar - so say so rather than ship that silently.
include(CheckIncludeFileCXX)
set(CMAKE_REQUIRED_INCLUDES ${SDL2_INCLUDE_DIRS})
check_include_file_cxx(X11/Xlib.h RAMKOLFX_HAVE_XLIB_H)
unset(CMAKE_REQUIRED_INCLUDES)
if(NOT RAMKOLFX_HAVE_XLIB_H)
    message(WARNING "X11/Xlib.h not found (install the libX11 development headers): "
                    "the GUI builds, but minimize-to-tray won't work on X11.")
endif()

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.6
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC PkgConfig::SDL2 OpenGL::GL)
# IM_ASSERT is assert(): only Debug builds should abort on one. Release and
# RelWithDebInfo get NDEBUG from CMake anyway, but distro builds use
# CMAKE_BUILD_TYPE=None (Arch, Debian) or none at all, which would otherwise
# ship ImGui's asserts, and its source paths, in the packaged GUI.
target_compile_definitions(imgui PUBLIC $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

add_executable(ramkolfx-gui
    src/main.cpp
    src/app.cpp
    src/autostart.cpp
    src/control_client.cpp
    src/desktop_exec.cpp
    src/fd_wakeup.cpp
    src/gui_prefs.cpp
    src/logo.cpp
    src/position_dial.cpp
    src/renderer_opengl3.cpp
    src/tray.cpp
    src/ui_widgets.cpp
    src/x11_window.cpp
)

target_link_libraries(ramkolfx-gui PRIVATE
    ramkolfx_common
    imgui
    PkgConfig::SDL2
    OpenGL::GL
    PkgConfig::FONTCONFIG
    PkgConfig::DBUS
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# The app icon (packaging/common/ramkolfx.svg, named by ramkolfx.desktop's
# Icon=) is generated from the same logo definition the GUI draws its
# window, header and tray icons from (src/logo.hpp). It's checked in rather
# than generated during the build, because the generator is compiled for
# the target and so can't run on the build host when cross-compiling.
# Instead the logo_svg_up_to_date test fails if the checked-in copy drifts
# from logo.hpp; `cmake --build build --target ramkolfx-update-logo-svg`
# regenerates it. The generator only needs ImGui's headers, not the library.
set(RAMKOLFX_LOGO_SVG ${PROJECT_SOURCE_DIR}/packaging/common/ramkolfx.svg)
add_executable(ramkolfx-logo-svg EXCLUDE_FROM_ALL tools/logo_svg.cpp src/logo.cpp)
target_include_directories(ramkolfx-logo-svg PRIVATE src ${imgui_SOURCE_DIR})
add_custom_target(ramkolfx-update-logo-svg
    COMMAND ramkolfx-logo-svg ${RAMKOLFX_LOGO_SVG}
    VERBATIM)
if(RAMKOLFX_BUILD_TESTS)
    set_target_properties(ramkolfx-logo-svg PROPERTIES EXCLUDE_FROM_ALL OFF) # ctest runs what ALL built
    add_test(NAME logo_svg_up_to_date COMMAND ramkolfx-logo-svg --check ${RAMKOLFX_LOGO_SVG})
endif()

install(TARGETS ramkolfx-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ramkolfx)
configure_file(${PROJECT_SOURCE_DIR}/packaging/common/ramkolfx-gui.1.in
               ${CMAKE_CURRENT_BINARY_DIR}/ramkolfx-gui.1 @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ramkolfx-gui.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 COMPONENT ramkolfx)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/common/ramkolfx.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT ramkolfx)
install(FILES ${RAMKOLFX_LOGO_SVG}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps COMPONENT ramkolfx)
set(RAMKOLFX_METAINFO ${PROJECT_SOURCE_DIR}/packaging/common/io.github.coldsteel48.RamkolFX.metainfo.xml)
install(FILES ${RAMKOLFX_METAINFO}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo COMPONENT ramkolfx)

# What distro tooling and software centres read: checked when the validators
# are installed, so packagers' test runs catch a broken file.
if(RAMKOLFX_BUILD_TESTS)
    find_program(APPSTREAMCLI appstreamcli)
    if(APPSTREAMCLI)
        add_test(NAME appstream_metainfo_valid
                 COMMAND ${APPSTREAMCLI} validate --no-net ${RAMKOLFX_METAINFO})
    endif()
    find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
    if(DESKTOP_FILE_VALIDATE)
        add_test(NAME desktop_entry_valid
                 COMMAND ${DESKTOP_FILE_VALIDATE} ${PROJECT_SOURCE_DIR}/packaging/common/ramkolfx.desktop)
    endif()
endif()
