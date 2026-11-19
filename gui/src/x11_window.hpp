// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

struct SDL_Window;

namespace ramkolfx::gui
{

// The two X11 window operations SDL2's API has no equivalent for. Both
// only ever ask the window manager for a state change through EWMH, so
// SDL hears of the result through its usual X events and its own window
// flags (MINIMIZED in particular, which Dear ImGui's SDL backend reads
// too) stay true - nothing here changes window state behind SDL's back.
//
// They use the libX11 that SDL's X11 backend already loaded rather than
// linking one in, so this adds no dependency. Each returns false (doing
// nothing) when the window isn't on X11.

// Takes the window off (or puts it back on) the taskbar, pager and window
// switcher, via _NET_WM_STATE_SKIP_TASKBAR/_SKIP_PAGER (plus KWin's
// _KDE_NET_WM_STATE_SKIP_SWITCHER, which other window managers ignore).
// Used to hide a *minimized* window to the tray: it stays minimized - SDL
// still knows it is - and is just no longer listed anywhere. SDL_HideWindow
// can't do this: a minimized X11 window is already unmapped, so SDL only
// flips its HIDDEN flag and the window stays in the taskbar.
bool SetX11WindowSkipTaskbar(SDL_Window* SdlWindow, bool bSkip);

// Brings a minimized X11 window back without touching its maximized
// state - SDL2's SDL_RestoreWindow un-maximizes it on X11 as well. Maps
// the window (ICCCM's Iconic -> Normal request) and asks the window
// manager to activate it via _NET_ACTIVE_WINDOW, for window managers that
// keep minimized windows mapped. The activation claims to come from a
// pager, so focus-stealing prevention doesn't block it: only call this for
// an explicit user request (a tray click, a second launch).
bool DeiconifyX11Window(SDL_Window* SdlWindow);

// Whether this build has the two above at all: false when it was built
// without Xlib's headers (CMake warns) or against an SDL without X11
// support - they then always return false. Where it's true, they work for
// any window on SDL's "x11" video driver.
bool IsX11WindowControlBuilt();

} // namespace ramkolfx::gui
