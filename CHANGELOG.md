# Changelog

All notable changes to RamkolFX are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
follows [Semantic Versioning](https://semver.org/) — see
[Versioning](README.md#versioning).

## [Unreleased]

## [0.4.0] - 2026-11-20

### Added

- **Auto-detect recognises games.** A new Game class in the Auto-detect
  table, with Action as its default preset. A game is recognised from
  the app, not the sound: Steam launched it, it runs through Wine or
  Proton, or it marks its own audio as a game's (SDL-based games do).
  So Discord or in-game dialogue no longer makes a game count as Speech.
  - *App binding:* an app bound to Auto-detect that is a game gets the
    Game preset.
  - *Device binding:* while any game plays, the Game preset applies,
    whatever else plays alongside it.
  - *No model needed:* the audio classifier doesn't run while a game is
    in effect.
  - *Holding:* tweaking the EQ holds your curve until the game stops.
  - *Quiet moments:* a game counts as playing for as long as it's open,
    so menus and loading screens don't flip the EQ back and forth.

  The app list marks games. Bind a game to a preset (FPS, RPG) to give
  it its own curve.
- **Priority when a game and music play together.** There is one EQ
  curve for everything, so a new Priority setting in the App preset card
  decides whose preset applies:
  - *Game first* (default): while a game is open, its preset plays. Bound
    music and video apps wait.
  - *Music first:* a bound app that plays takes over from the game; the
    game's preset comes back when it stops.
  - *Latest first:* whichever app started playing most recently wins.

  It's also in Settings and in the tray menu (to change in the middle of
  a game); all three stay in step. The
  card lists the bound apps that are waiting. Only bound apps take
  part: an unbound music app never takes over, and the card says so under
  Music first.

## [0.3.0] - 2026-11-20

### Added

- **Man pages** for `ramkolfxd` and `ramkolfx-gui` (`man ramkolfxd`).
- **AppStream metadata,** so software centres such as GNOME Software and
  KDE Discover can list RamkolFX with its description and screenshots.
- **Auto-detect timing sliders.** The expanded Auto-detect card has
  four new sliders, so you can trade reaction speed against stability
  and CPU use:
  - *Check every:* how often detection listens. The default is 2 s, and
    3x that once the class has settled.
  - *Switch after:* how long new content has to stand out before the EQ
    switches (default 4 s).
  - *Keep at least:* the minimum time between two switches (default
    10 s; skipped after a gap between tracks).
  - *CPU limit:* the most of one core detection may use (default 1%).

  The daemon saves them in `settings.conf`. Each check costs about 10 ms
  of CPU, so at the default 1% limit checks can't come more often than
  about once a second. When the limit is what holds detection back, the
  slider says so.

### Changed

- **The Arch, Debian and Gentoo packages use your distro's libmysofa and
  KissFFT,** and the Fedora package its libmysofa, instead of copies built
  into RamkolFX, so they get your distro's security fixes. Building from
  source still uses the pinned copies unless you pass
  `-DRAMKOLFX_SYSTEM_MYSOFA=ON` / `-DRAMKOLFX_SYSTEM_KISSFFT=ON`.
- **The control window is now one real window.** The controls fill the
  whole window, instead of sitting in a panel inside it that could be
  dragged off-screen. A larger window gets a wider layout, and one
  narrower or shorter than the controls scrolls. The GUI no longer writes
  an `imgui.ini` file into the folder it was started from.

## [0.2.2] - 2026-11-20

### Changed

- **Changing the EQ by hand now pauses Auto-detect only until the content
  changes,** not for as long as the app keeps playing.
  - *After a gap of silence:* detection checks what plays next. If it maps
    to a different preset than what you tuned for, that preset applies. If
    it's the same kind of content (a paused video resuming, a quiet
    passage, another song of the same genre), your curve stays.
  - *While paused:* detection only watches for silence. It runs no
    classifier, and unloads it after 5 minutes. (If you tune before
    anything was detected, or tune again while paused, it first runs
    briefly to learn what you tuned for.)
  - *When the app stops:* a preset you picked stays, as before - unless
    Auto-detect has switched presets again since, in which case the EQ
    returns to what it was before the app started, as with any binding.

## [0.2.1] - 2026-11-20

### Fixed

- Auto-detect never worked from the 0.2.0 packages: they didn't pull in
  ONNX Runtime, and on Debian the daemon didn't find it even when it was
  installed (Debian ships only `libonnxruntime.so.1.21`).
  - **Dependencies now:**
    - Arch: `onnxruntime` (any of `onnxruntime-cpu` / `-cuda` / `-rocm`).
    - Debian: `libonnxruntime1.21`.
    - Fedora: `onnxruntime`.
    - Alpine: `onnxruntime`.
    - Nix: the flake links its own copy.
  - **Lookup:** the daemon finds versioned library names as well.
  - **Gentoo:** ::gentoo has no ONNX Runtime, so it stays a manual install.

### Changed

- The Alpine package is built on Alpine 3.23, the first release that
  ships `onnxruntime`.

## [0.2.0] - 2026-11-20

### Added

- **Auto-detect (AI) EQ presets (experimental).** Bind an app (say, your
  browser) or an output device to "Auto-detect (AI)" instead of a fixed
  preset. A local audio classifier then listens to what's playing and
  picks the preset for it.
  - **Choosing the preset:** an editable class -> preset table maps
    Speech -> Spoken Word, Movie -> Movie, and for music the best-scoring
    genre -> its genre preset.
  - **Switching:** it switches within a few seconds of a new song (a gap
    of silence marks one), and waits out ambiguous stretches.
  - **Manual changes:** changing the EQ by hand pauses it for that app's
    session.
  - **Requirements:** ONNX Runtime 1.17 or newer, which is loaded at run
    time. Without it, Auto-detect shows as unavailable.
  - **Model:** EfficientAT `mn10_as` (MIT).
  - **Hardware and cost:** it runs on the CPU, or on an Intel NPU via
    OpenVINO when one validates against the CPU; the GPU is opt-in. It
    stays under 1% of a core at the lowest scheduling priority, and frees
    its memory after 5 idle minutes.
  - **GUI:** Auto-detect entries in the app and device preset cards, a
    live "AI: Jazz 82%" readout, and an Auto-detect card with the device
    choice, CPU use and the class table.
  - **Known limit:** genre picks are a weak guess for now (the model is
    much better at speech / music / film than at genres).
- **Channel activity on the Spatial tab:** a toggle animates each speaker
  on the dial by the live level of its own 7.1 channel (the LFE lights
  the center), so you can see which channels the audio actually uses.
- `ramkolfx-detect-bench` (built, not installed) measures Auto-detect
  against its CPU, memory and audio-thread budget. Setting
  `RAMKOLFX_DETECT_DUMP=<dir>` on the daemon logs each detection run's top
  labels and saves the audio window it judged.

### Changed

- The control protocol gained requests 0x1A-0x1E and 0x20 and replies
  0x89 and 0x90. The new fields are optional on the wire, so a 0.1.x GUI
  keeps working with this daemon. A 0.2.0 GUI talking to an older daemon
  disables the new features and asks for a daemon restart.

## [0.1.3] - 2026-11-20

### Added

- Built-in EQ presets for music and game genres, next to the general ones:
  Treble Boost, Loudness, Late Night, Spoken Word, Movie; Acoustic, Blues,
  Classical, Electronic, Hip-Hop, Jazz, Metal, Pop, R&B, Reggae, Rock;
  Action, FPS, Horror, Racing, RPG, RTS. The preset pickers group them
  under General / Music / Games headers, with your own presets last.

### Changed

- A saved preset with the same name as one of the new built-ins (say, your
  own "Rock") is kept as "Rock (custom)", with its device/app bindings,
  instead of being dropped. If it was the active preset it stays active
  under the new name, and it is never mistaken for the built-in.
- Bass Boost is a little gentler (+5/+4/+3/+1.5 dB instead of
  +6/+5/+4/+2) so its overlapping bands peak at +6 dB rather than +7 dB.
  No built-in curve now boosts more than 6 dB anywhere.

## [0.1.2] - 2026-11-20

### Fixed

- The GUI window showed a generic icon in the title bar and taskbar instead
  of the RamkolFX logo. It now identifies itself as `ramkolfx` (Wayland
  app_id / X11 WM_CLASS) to match `ramkolfx.desktop`, sets its own window
  icon, and an app icon (`hicolor/scalable/apps/ramkolfx.svg`) is installed.
  The window, tray, in-app header and installed icon are all drawn from one
  logo definition. Start-at-login entries created by 0.1.1 get the icon
  added on the next launch.

## [0.1.1] - 2026-11-19

### Added

- GUI Settings popup: start at login (XDG autostart, launches with the new
  `--minimized` flag), minimize to the system tray on close / on minimize.
- System tray icon (StatusNotifierItem over D-Bus) with Show/Hide and Quit.
- Single-instance GUI: launching it again shows the running window.
- `--version` on both binaries; version shown in the GUI header.
- `VERSION` file as the single source of truth, with `packaging/version.sh`
  to sync and check the distro recipes against it.

### Changed

- The GUI now links libdbus-1 (new build dependency).
- The GUI now requires SDL2 >= 2.0.18, checked at configure time, and
  libX11 dev headers for minimize-to-tray on X11 (warned about if missing).
- The GUI stops drawing while minimized instead of rendering at full speed.

## [0.1.0] - 2026-11-18

First release. Feature set at this point:

- Virtual PipeWire sink with live processing and output to a real device.
- Spatial audio: ambisonics speaker virtualisation and HRTF binaural
  ("Advanced") mode with a switchable HRTF catalog.
- 10-band graphic EQ, psychoacoustic bass enhancer, named EQ presets with
  per-device / per-app auto-switching, per-device spatial mode.
- Dear ImGui control app (`ramkolfx-gui`) and `ramkolfxd` daemon, talking over
  a Unix control socket.
- Packages: Debian, RPM, Arch, Alpine, Gentoo ebuild, Nix flake.

[Unreleased]: https://github.com/Coldsteel48/RamkolFX/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/Coldsteel48/RamkolFX/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/Coldsteel48/RamkolFX/compare/v0.2.2...v0.3.0
[0.2.2]: https://github.com/Coldsteel48/RamkolFX/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/Coldsteel48/RamkolFX/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/Coldsteel48/RamkolFX/compare/v0.1.3...v0.2.0
[0.1.3]: https://github.com/Coldsteel48/RamkolFX/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/Coldsteel48/RamkolFX/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/Coldsteel48/RamkolFX/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/Coldsteel48/RamkolFX/releases/tag/v0.1.0
