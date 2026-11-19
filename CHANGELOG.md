# Changelog

All notable changes to RamkolFX are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
follows [Semantic Versioning](https://semver.org/) — see
[Versioning](README.md#versioning).

## [Unreleased]

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

[Unreleased]: https://github.com/Coldsteel48/RamkolFX/compare/v0.1.2...HEAD
[0.1.2]: https://github.com/Coldsteel48/RamkolFX/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/Coldsteel48/RamkolFX/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/Coldsteel48/RamkolFX/releases/tag/v0.1.0
