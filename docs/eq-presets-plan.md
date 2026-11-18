# Named EQ presets with device/app auto-switching (Roadmap item 7)

## Context

The protocol already decoded `SetHwEqPreset`/`SaveHwEqPreset`/
`SetContentEqPreset`/`SaveContentEqPreset`/`GetEqPresetCatalog`/
`GetContentStreams`, but the daemon had no handlers and the GUI showed
disabled placeholder cards ("Device preset" / "App preset").

Key constraint: every app plays into **one** virtual sink, so the daemon
only ever sees a single mixed signal. True per-app EQ (each stream EQ'd
before mixing) would need a per-stream capture path - a major pipeline
redesign. Decision: **per-app presets mean auto-switching** the single
global EQ to the preset bound to whichever app is playing. This also
gives item 8 (ONNX genre auto-detect) the switching machinery for free.

## Behavior

- A **preset** is a named snapshot of the HW EQ curve (all bands) plus
  the bass enhancer settings.
- A preset can be **bound** to an output device and/or to any number of
  apps (keyed by PipeWire `application.name`).
- **Resolution rule:** most-recently-started bound app that is playing
  wins; else the current output device's bound preset; else leave the
  manual curve alone. A ~2s grace period keeps a briefly paused app from
  flipping the EQ back and forth.
- Tweaking a slider while a preset is active marks it **"Custom
  (unsaved)"**; the stored preset is not modified until saved.
- Built-in presets (Flat, Bass Boost, Vocal) are read-only; "Save as" on
  one creates a copy, Delete is refused.
- Switching is **edge-triggered**: the rule is re-evaluated every 250ms
  (and right after any command that can change it), but a preset is only
  applied when the resolved target *changes* - so tweaks made while a
  binding is in effect aren't stomped on the next tick. If the target's
  preset is already the active one, it isn't re-applied (keeps unsaved
  tweaks, e.g. when binding the device to the preset you're adjusting).
- Going from "no binding applies" to any binding **snapshots** the EQ
  state; going back to "no binding applies" **restores** it - so when a
  bound app stops, the sound returns to what it was before.
  Exceptions: explicitly unbinding the binding currently in effect keeps
  the current sound (a device binding underneath is adopted without
  applying its preset), and picking/saving a preset by hand while a
  binding is in effect makes that the new baseline. Both re-capture the
  snapshot from the current sound rather than dropping it, so a device
  binding underneath still has a baseline to return to. Re-picking (to
  revert tweaks) or saving over the binding's *own* preset is not a new
  baseline: the binding stays in charge and the sound still returns to
  what it was before the binding once it ends. The snapshot is in memory
  only.
- A preset or binding change that can't be written to `presets.conf` is
  rolled back (logged), so the catalog never claims what a restart would
  lose.
- Deleting a preset removes every binding to it.

## Status

All six phases done. Verified end to end against real PipeWire with
`pw-play` streams: app override with grace period and snapshot restore,
device binding, app over device, explicit unbind, delete-drops-binding.
Unit tests: `./build.sh test` (protocol round trips, preset store and
bindings, the auto-switch rule and its snapshot/restore state machine
`AutoSwitchController`, per-device spatial modes) - the pure preset code lives in the
`ramkolfx_presets` library so the daemon and tests link the same
objects. The end-to-end PipeWire checks (`tests/e2e/run.sh`) are not
part of ctest: they need a running PipeWire session and start a real
daemon (with a throwaway config directory).

Known limits: the GUI's app card lists only apps currently routed into
RamkolFX, so a binding for an absent app can't be edited until it shows
up again; the pre-binding snapshot is in memory only.

## Protocol changes

- Presets are addressed **by name, not index**: deleting a preset shifts
  later indices, which would silently retarget bindings. GUI and daemon
  ship from the same repo, so the wire format can change freely.
- New `DeleteEqPreset` and `BindDevicePreset` opcodes;
  `SetContentEqPreset` became `BindAppPreset` (empty preset = unbind);
  `SetContentEqBand`/`SaveContentEqPreset` retired (true per-app EQ is
  out of scope).
- `ContentStreamListResponse` entries carry `bPlaying` and the app's
  bound preset; `Status` carries the current device's bound preset and
  why the active preset is active (manual / device / app + app name).
- `EqPresetCatalogResponse` entries carry a built-in flag.
- `Status` carries `ActivePresetName` and `bPresetModified`, plus
  `EqRevision` / `PresetCatalogRevision` counters that tell a client to
  re-fetch the curve / the preset list after a change made elsewhere.

## Phases

1. **Preset store + core opcodes.** `PresetStore` persisting
   `~/.config/ramkolfx/presets.conf` (same key=value, temp-file-rename
   style as `SettingsStore`); Get catalog / Apply / Save / Delete by name;
   active preset + modified flag in Status and persisted settings;
   built-in presets; GUI preset combo, Save-as, Delete.
2. **Device binding.** Bind/unbind a preset to an output device ("Global
   default" toggle = unbound); apply on output-device change, including
   the startup fallback in `ResolveInitialOutputDevice`.
3. **Stream tracking.** Extend `DeviceRegistry` to track
   `Stream/Output/Audio` nodes routed into our virtual sink
   (`application.name`, running state); implement `GetContentStreams`.
   Riskiest phase - verify against real apps (Firefox, mpv) first.
4. **App binding + auto-switch rule.** Resolution as a pure,
   unit-testable function, with the grace period.
5. **GUI app card.** App list from `GetContentStreams`, bind/unbind.
6. **Tests.** CTest target: protocol round-trips, `PresetStore`
   load/save, resolution rule.
