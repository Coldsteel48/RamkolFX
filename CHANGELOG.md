# Changelog

All notable changes to RamkolFX are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
follows [Semantic Versioning](https://semver.org/) — see
[Versioning](README.md#versioning).

## [Unreleased]

### Added

- GUI Settings popup: start at login (XDG autostart, launches with the new
  `--minimized` flag), minimize to the system tray on close / on minimize.
- System tray icon (StatusNotifierItem over D-Bus) with Show/Hide and Quit.
- Single-instance GUI: launching it again shows the running window.

### Changed

- The GUI now links libdbus-1 (new build dependency).
- The GUI now requires SDL2 >= 2.0.18, checked at configure time, and
  libX11 dev headers for minimize-to-tray on X11 (warned about if missing).
- The GUI stops drawing while minimized instead of rendering at full speed.

## [0.1.0-alpha.1] - 2026-11-18

First alpha. Feature set at this point:

- Virtual PipeWire sink with live processing and output to a real device.
- Spatial audio: ambisonics speaker virtualisation and HRTF binaural
  ("Advanced") mode with a switchable HRTF catalog.
- 10-band graphic EQ, psychoacoustic bass enhancer, named EQ presets with
  per-device / per-app auto-switching, per-device spatial mode.
- Dear ImGui control app (`ramkolfx-gui`) and `ramkolfxd` daemon, talking over
  a Unix control socket.
- Packages: Debian, RPM, Arch, Alpine, Gentoo ebuild, Nix flake.
- `--version` on both binaries; version shown in the GUI header.

[Unreleased]: https://github.com/Coldsteel48/RamkolFX/compare/v0.1.0-alpha.1...HEAD
[0.1.0-alpha.1]: https://github.com/Coldsteel48/RamkolFX/releases/tag/v0.1.0-alpha.1
