// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

namespace ramkolfx::gui
{

// Preferences that belong to this GUI instance alone and are never sent
// to the daemon (see App::Preferences) - kept in their own file
// rather than folded into the daemon's SettingsStore so a headless
// client or a second GUI instance isn't affected by one GUI's local UX
// choices.
struct GuiPreferences
{
    // Dragging a left/right speaker also drags its mirrored counterpart -
    // see DrawPositionDial's bMirrorEnabled.
    bool bMirrorModeEnabled = false;

    // Whether the HW EQ panel shows per-band frequency/Q/filter-type
    // controls (advanced) or just the 10 ISO-labeled gain sliders
    // (simple). Purely a display choice - the daemon's wire format is
    // already fully parametric either way, see protocol.hpp's EqBand.
    bool bAdvancedEqMode = false;

    // Whether the bass enhancer panel shows Cutoff/Drive controls
    // (advanced) or just the Enabled toggle and Amount slider (simple).
    // Purely a display choice, same reasoning as bAdvancedEqMode - see
    // protocol.hpp's BassEnhancerSettings.
    bool bAdvancedBassMode = false;

    // The speaker dial animates each speaker by the live level of the
    // channel feeding it (see App::Tick / DrawPositionDial's Activity),
    // instead of drawing them static. Polls the daemon ~30x/s while on and
    // the Spatial tab is showing.
    bool bShowChannelActivity = false;

    // Which of the two top-level tabs (0 = Spatial Audio & Speakers, 1 =
    // Equalizer & Bass) is showing (and was when the GUI last closed) -
    // only one tab's DrawXxxTab() runs per frame, so the AlwaysAutoResize
    // window only ever measures whichever tab is active. Purely a local UX
    // convenience, never sent to the daemon.
    int ActiveTabIndex = 0;

    // Whether the Equalizer tab's Auto-detect card is expanded (class
    // table and timing sliders showing). Local UX only, like the tab.
    bool bAutoDetectExpanded = false;

    // Window-close / window-minimize hide the window to the system tray
    // instead of quitting / iconifying it - see main.cpp's event loop. Only
    // honored while the tray icon is actually up (see Tray::IsIconShown),
    // so a desktop without a tray never ends up with an unreachable window.
    bool bMinimizeToTrayOnClose = false;
    bool bMinimizeToTrayOnMinimize = false;

    // The one copy of which tray options are in effect - App applies it
    // to the live Settings toggles, main.cpp to the saved ones at startup,
    // before an App exists. Minimize-to-tray only counts where it can work
    // (an X11 session, in a build with X11 support - see
    // App::MinimizeToTrayBlocker).
    bool MinimizesToTrayOnMinimize(bool bMinimizeToTraySupported) const
    {
        return bMinimizeToTrayOnMinimize && bMinimizeToTraySupported;
    }
    // Either option in effect: the tray icon should be up.
    bool WantsTrayIcon(bool bMinimizeToTraySupported) const
    {
        return bMinimizeToTrayOnClose || MinimizesToTrayOnMinimize(bMinimizeToTraySupported);
    }
};

// Reads $XDG_CONFIG_HOME/ramkolfx/gui_prefs.conf (falling back to
// ~/.config/ramkolfx/gui_prefs.conf), or GuiPreferences{} defaults if the
// file doesn't exist or can't be read.
GuiPreferences LoadGuiPreferences();

// Writes InPreferences to the same path Load() reads from. Best-effort:
// logs to stderr and returns on failure rather than throwing.
void SaveGuiPreferences(const GuiPreferences& InPreferences);

} // namespace ramkolfx::gui
