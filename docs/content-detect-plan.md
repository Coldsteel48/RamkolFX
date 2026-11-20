# Content-aware EQ: AI auto-detect as a binding (Roadmap item 8)

## Context

Item 7 (`docs/eq-presets-plan.md`) gave the daemon named presets,
device/app bindings and an edge-triggered auto-switcher. Item 8 adds a
local ONNX audio classifier that listens to what's playing and picks a
preset for it (speech -> Spoken Word, jazz -> Jazz, a film mix -> Movie).

Key decision: auto-detect is **not a global mode**, it's a **binding
kind**. An app or output device can be bound to a fixed preset (as today)
or to **Auto-detect (AI)**: "Firefox -> Auto, Spotify -> Rock, everything
else unbound". This keeps it opt-in where the user wants it, reuses the
resolution rule and snapshot/restore unchanged, and means the classifier
only runs while an Auto binding is in effect.

Naming: "Content" already means *apps* in this codebase
(`ContentStreamInfo`, `ContentAppName`), so the new code says "Detected"/
"AutoDetect".

Everything runs locally; no audio or results leave the machine.

## Behavior

- **Binding kinds.** An app or device binding is either a preset name or
  Auto-detect. The two are mutually exclusive per target: binding a preset
  clears Auto and vice versa. The GUI's binding combos get an
  "Auto-detect (AI)" entry above the preset categories.
- **Resolution rule unchanged:** most-recently-started bound app that is
  playing wins (2s grace); else the current output device's binding; else
  manual. If the winning binding is Auto, its preset comes from the
  **class table** (below) via the currently detected class.
- **Pending.** While an Auto binding is in effect but no class is
  confident yet, the target is `{Source, Key, ""}`: the binding is in
  effect (the pre-binding snapshot is taken) but the curve is left alone.
  `AutoSwitchController::Evaluate` already behaves this way for a target
  whose preset doesn't resolve. A confident class turns the target into
  `{Source, Key, "<preset>"}` and the normal edge-triggered switch applies
  it.
- **Fresh detection per binding.** The smoother resets whenever the
  winning Auto target changes (another app wins, the app restarts after
  the grace period, the output device changes). Within one playing spell
  the last confident class is held through silence and quiet passages.
- **Reaction time vs stability.** Inference only judges a window that is
  sound throughout (the model's input, ~1 s), and a class must hold for
  ~4 s to be adopted: the first class lands ~5 s into a spell. A change
  needs the new class to lead (score EMA, ~3 s time constant, with
  hysteresis toward the current class) for ~4 s, and at least ~10 s
  since the last change so a genre-ambiguous song doesn't flip the EQ
  back and forth. A **track boundary** - silence of >= 1.5 s, the gap
  between songs, measured on the audio itself in 100 ms chunks - forgets
  the old track's scores and waives that 10 s, so a new song switches
  ~5 s in. Gapless/crossfaded changes take ~6-14 s. All values tuned in
  phase 0, and adjustable in the GUI (see `SetAutoDetectTiming`).
- **Tweaks pause detection until the content changes.** Moving a slider
  or picking/saving a preset by hand while an Auto binding is in effect
  holds detection. The resolver returns the pending target
  `{Source, Key, ""}`, which leaves the curve as the user set it.
  - *While held:* the worker keeps tracking silence but runs no
    inference, and unloads the classifier after `IdleUnloadMs`.
  - *Learning:* if nothing was classified when the user tuned, or the
    class may be on its way out (a boundary just passed, a rival class
    confirming), inference runs until a class is confirmed: that is what
    they tuned for. Re-tuning while held or during a re-check learns the
    new content the same way (while held, detection is paused, so a
    gapless change would otherwise go unseen).
  - *Boundaries vs. the last tune:* each track boundary is stamped when
    its audio reached the tap, not when the worker drained it, and is
    placed against the last time the user tuned. One before it (collected
    late) means they tuned for what followed: learn that. One after it
    starts a re-check. If one ends the content while it is still being
    learned, the class the scores pointed at stands in for what they
    tuned for (nothing at all: the hold ends).
  - *Re-check:* inference resumes on what plays after the boundary. Once
    it is classified, the hold ends if its class maps to a different
    preset than the class the user tuned for; otherwise it carries on
    until the next boundary. So a paused video or a quiet passage keeps
    the user's curve. The status reports detection as paused only while
    held, not while learning or re-checking.
  - A new binding or playing spell also ends the hold.
  - *Baseline:* a hand pick under an Auto binding becomes the baseline as
    with any binding, but the one it replaced is kept aside
    (`OnPresetChosenByHand(..., bBindingMayResume)`). If a binding
    actually switches the curve again (applies a different preset), that
    pre-binding baseline is restored as the one to return to; a binding
    that leaves the pick playing keeps the pick. An explicit takeover
    (unbind, delete) drops it.
- **When the Auto binding stops applying**, the sound returns to the
  snapshot, exactly like any other binding.
- **Runtime or model missing.** Auto entries are disabled in the GUI with
  a tooltip saying what's missing. Existing Auto bindings count as
  unbound (resolution falls through to the next rule) and the GUI marks
  them with a warning.
- **Known limit:** the model hears the whole mix, not just the winning
  app. A second, unbound app playing at the same time influences
  detection. Separating them needs per-stream capture, which item 7
  already ruled out.

## Classes and the class table

Two-level: a coarse class first, a genre within Music only above a
higher threshold. AudioSet labels fold into classes through a static
`LabelMap`; the exact label lists and thresholds are fixed in phase 0.

| Class    | AudioSet labels folded in (approx.)                                   | Default preset |
|----------|-----------------------------------------------------------------------|----------------|
| Speech   | Speech, Narration, Conversation                                       | Spoken Word    |
| Movie    | Film cues summed (weapons, vehicles, impacts, weather, outdoor ambience; see below), with or without Speech/Music | Movie |
| Music    | Music, no confident genre                                              | (no switch)    |
| Rock     | Rock music, Rock and roll, Punk, Grunge, Progressive/Psychedelic rock | Rock           |
| Metal    | Heavy metal                                                           | Metal          |
| Pop      | Pop music, Disco                                                      | Pop            |
| Hip-Hop  | Hip hop music                                                         | Hip-Hop        |
| R&B      | Rhythm and blues, Soul, Funk, Gospel                                  | R&B            |
| Electronic | Electronic, House, Techno, Dubstep, Drum and bass, Trance, EDM      | Electronic     |
| Jazz     | Jazz, Swing                                                           | Jazz           |
| Classical | Classical music, Opera                                               | Classical      |
| Blues    | Blues                                                                 | Blues          |
| Reggae   | Reggae, Ska                                                           | Reggae         |
| Acoustic | Folk, Country, Bluegrass, Acoustic guitar                             | Acoustic       |
| Game     | None: decided from stream metadata, not audio (see Game detection)    | Action         |

- The class table is **global** (shared by every Auto binding) and
  user-editable: any class can point at any preset, user presets
  included, or at nothing ("no switch").
- No new built-in presets: the General/Music/Games built-ins already
  cover every class.
- Games aren't recognised from audio: a game sounds too much like a film.
  The Game class comes from what PipeWire says about the stream instead
  (phase 6, below).

## Game detection (phase 6)

A game is recognised from the stream's metadata and its process, not
from the audio: that is cheap, reliable, and immune to Discord or game
dialogue being heard as Speech.

**Signals.** A playback stream counts as a game if any of these holds:

1. **`media.role` is `Game`** (any case). SDL's audio backends set this
   by default (to confirm on a real game), so most native Linux games
   and emulators are covered.
2. **A Wine process.** The process binary is `wine`, `wine64`,
   `wine-preloader`, `wine64-preloader`, or ends in `.exe`. That covers
   Proton, Lutris, Heroic and Bottles. A non-game run through Wine (a
   music player, say) is a false positive; the user binds it to a preset
   to override.
3. **Steam launched it.** The process environment has a non-empty
   `SteamAppId` or `SteamGameId` other than `0`, read from
   `/proc/<pid>/environ`.

**Which process.** Measured on PipeWire 1.4:

- Streams through the PulseAudio layer (`pipewire-pulse`; Wine and most
  apps) carry `application.process.id` and `application.process.binary`
  on the node. Their client object's `pipewire.sec.pid` is
  `pipewire-pulse` itself, so it is useless.
- Native PipeWire streams carry no `application.*` keys on the node; the
  client object's `pipewire.sec.pid` (from the socket's credentials,
  set by the server) is the real process.
- So: the node's `application.process.id`, else the client's
  `pipewire.sec.pid`. The binary is the node's
  `application.process.binary`, else the basename of `/proc/<pid>/exe`.
- A client-reported pid may belong to another pid namespace (a Flatpak
  or Steam container). The environment is only read when the basename of
  `/proc/<pid>/exe` matches the reported binary, so a stranger's
  environment is never read in its place. Signals 1 and 2 don't need
  `/proc` at all.

**Where.** `DeviceRegistry` already binds every playback stream for its
state. It records each client's `pipewire.sec.pid` from the registry
and decides "game or not" once per stream when the node's properties
arrive (again if they change). The decision is pure
(`game_detection.*`, with `/proc` behind a root path the tests can
fake). `ContentStreamInfo` gains `bGame`.

**Resolution.** The resolution rule is unchanged; the resolved binding
gains `bGame`:

- An **app** Auto binding: the winning app is a game.
- A **device** Auto binding: any app that is playing (or within the 2 s
  grace) is a game. Whether it beats a bound app playing alongside it is
  the Priority setting's call (Game first by default); see
  [docs/playback-priority-plan.md](playback-priority-plan.md).
- **A game counts as playing while its stream is open**, not only while
  it runs. Games go quiet for seconds at a time (menus, loading,
  alt-tab), and with the 2 s grace the EQ flipped between the Game preset
  and the previous curve all session (seen with Arma Reforger). This
  applies to every binding of a game, preset bindings included.

`bGame` is part of the binding's identity, so a game starting or
stopping under a device Auto binding starts detection afresh, like a
new binding.

**While a game is in effect** (`AutoDetectSession`):

- The class is Game, at 100%, straight away: no confirm time.
- The audio model doesn't run, so the worker stays off: no tap, no
  inference, no silence tracking.
- Tweaking the EQ holds the curve until the game stops (the binding
  changes). There are no track boundaries to re-check on.

**Wire and storage.**

- `DetectedClass::Game` is a new value after the genres. `IsGenreClass`
  becomes an explicit `Rock..Acoustic` range rather than "everything
  after Rock".
- The class table stores it as `detected.game` in `presets.conf`; its
  default preset is Action.
- `ContentStreamListResponse` gets a second trailer block: one `bGame`
  byte per entry after the `bAutoDetect` block. Older GUIs ignore it,
  and an older daemon's list decodes as all false.
- An older GUI shows the new class as "Unknown".

**GUI.** The Game row appears in the class table like any other class.
The app list marks streams detected as games.

**Later, not in phase 6:**

- **Game genre.** Per-app binding already covers this: bind the game to
  FPS or RPG. An optional online lookup would map `SteamAppId` to the
  Steam store's genres, then to a Games preset (phase 11). That sends
  data off the machine, so it must be opt-in.
- **Overriding a false positive.** A "not a game" flag per app, if
  binding to a preset turns out not to be enough.
- **Games without ONNX Runtime.** The Game class needs no model, so Auto
  bindings could work for games even when the runtime is missing. Today
  the GUI disables Auto entries then, so this waits.
- **More launchers.** Environment markers from other launchers (umu,
  Lutris, Heroic), if Wine and `media.role` miss real cases.

## Architecture

```
RT thread: AudioEngine::HandleVirtualSinkAudio
  after the input memcpy, before ApplySpeakerGains / stage / EQ:
  ContentDetector::Tap: if enabled, downmix 7.1 -> mono, push to RingBuffer<float> (4 s @ 48k)
        |
ContentDetector worker (SCHED_IDLE; blocked unless an Auto binding is in effect)
  every 0.5 s: drain -> 100 ms chunks -> SilenceTracker (silence, track boundaries)
  when InferenceScheduler says so and the last window is all sound:
  window -> Resampler 48k -> model rate -> ContentClassifier::Classify -> AudioSet scores
  -> LabelMap::Fold -> DetectionEvent {scores | track boundary}
        |
AutoSwitch timer (existing 250 ms tick)
  ContentDetector::TakeEvents -> AutoDetectSession (DetectionSmoother -> current class)
  ResolveBinding(Activity, Catalog, OutputDevice, bAutoDetectAvailable)
  -> AutoDetectSession::Update (class table -> preset, or pending)
  -> AutoSwitchController::Evaluate
  ContentDetector::SetActive(ShouldDetect, spell) / SetSettled(IsSettled)
```

- **Tap before the EQ.** Classifying our own output would create a loop
  (EQ change -> different classification -> EQ change). The tap is a
  single atomic flag check when off.
- **RT side only downmixes and pushes** into the existing SPSC
  `RingBuffer`: no allocation, no locks.
- **Preprocessing lives in the ONNX graph** (STFT/mel, opset >= 17): the
  model takes a raw mono waveform, so C++ can't drift from the mel
  parameters the model was trained with. C++ does downmix + resample.
- **Pure, unit-testable pieces** go in `ramkolfx_presets` alongside the
  auto-switch logic: `LabelMap`, `DetectionSmoother` (clock-injected),
  the resolver changes, class-table/Auto-binding persistence.
- **Inference behind an interface** (`ContentClassifier`, same swappable
  idea as `DspStage`), with the ONNX Runtime implementation in the daemon
  (`ramkolfx_detect`, `daemon/src/detect/`); tests drive the worker with a
  fake.
- **The model describes itself** in ONNX metadata, so phase 0 can swap
  models without code changes: `ramkolfx.sample_rate`, `ramkolfx.labels`
  ('\n'-separated, output order), optional `ramkolfx.input_samples` (for a
  dynamic input length) and `ramkolfx.output` = `logits` (sigmoid applied
  in C++). Files: `audio_classifier.onnx` (fp32) and optional
  `audio_classifier.int8.onnx` (NPU) in the model directory
  (`RAMKOLFX_MODEL_DATA_DIR` at configure time, `RAMKOLFX_MODEL_DIR` in the
  environment).

## ONNX Runtime and hardware selection

- **Loaded with dlopen, not linked.** The whole ORT C API hangs off one
  exported symbol (`OrtGetApiBase`), so we vendor the MIT-licensed
  `onnxruntime_c_api.h` (1.21's, self-contained;
  `daemon/third_party/onnxruntime/`), dlopen `libonnxruntime.so[.1]` on
  first use, and request C API version 17 - everything we call exists
  since 1.17, so any runtime from 1.17 on works (older ones return null ->
  "too old", treated as unavailable). Once loaded the library stays
  mapped: sessions are released when idle (that's the memory that
  matters), but `dlclose()` of a C++ runtime with static state is a known
  source of crashes at exit, and its code pages are file-backed anyway. Builds and CI need no ORT; packages list
  it as optional (`optdepends` / `Recommends` / `Suggests` / a Gentoo USE
  flag / a flake option). `RAMKOLFX_ORT_LIB` overrides the library path
  for a custom ORT build. Packaged on every target distro: Arch
  (`onnxruntime-cpu`, `-cuda`, `-rocm`, ...), Debian 13
  (`libonnxruntime1.21`), Fedora, Alpine (community).
- **Execution provider policy:** setting `auto_detect.device = auto | cpu |
  gpu | npu` (GUI: "Run detection on", showing the provider actually in
  use, e.g. "NPU (OpenVINO)").
  - `auto`: NPU if one is available and passes validation, else CPU.
  - GPU is opt-in only, never chosen by `auto`. v1 drives CUDA and
    OpenVINO (Intel GPUs); AMD (ROCm/MIGraphX) EPs take plain option
    structs whose layout changes between ORT versions, unsafe to pass
    across a dlopen boundary, so AMD GPUs run on the CPU for now. The model is small
    (~5M params, one 1-2 s window every 2 s - an estimate of a few ms
    per run on a modern core, to be measured in phase 0), so GPU
    transfer/launch overhead exceeds the compute, CUDA/ROCm contexts
    cost hundreds of MB of VRAM and seconds to start, and on hybrid
    laptops a job every 2 s keeps the dGPU from runtime-suspending,
    which can cost several watts of battery all the time.
  - NPU in practice on Linux today: Intel NPUs via the OpenVINO EP (needs
    an OpenVINO-enabled ORT build and `intel-npu-driver`). AMD XDNA and
    Qualcomm QNN on Linux are immature; ORT's own `PREFER_NPU`
    selection policy is Windows-only for now, so we select ourselves from
    `GetAvailableProviders()`.
- **Validation before trusting an accelerator:** warm up, run a fixed
  test signal on it and on the CPU, and keep it only if every score is
  within 0.1 of the CPU's (NPU int8 can drift) and its median run is
  both faster and cheaper in CPU time. Otherwise fall back to CPU and log
  why.
- **Runtime failures** (driver reset, device lost) rebuild the session on
  CPU.
- **Two model files:** fp32 (CPU/GPU) and static-shape int8 QDQ (NPU),
  checked against each other in tests.
## Performance budget

Detection must never be noticeable: not as audio glitches, not as CPU
load, not as battery drain. These are hard limits, enforced by the code
below and checked in phase 0 and phase 2. Numbers marked (tbc) are
confirmed or tightened by phase 0's measurements.

| Resource | Budget |
|----------|--------|
| RT thread, detection off | one atomic load per block |
| RT thread, detection on | downmix + ring push, < 1% of the block's time |
| Worker CPU, average | <= 0.5% of one core on the reference *old* machine (tbc) |
| Worker CPU, hard cap | 1% duty cycle (inference time / interval), enforced |
| Memory | <= ~60 MB RSS added while a session is loaded (tbc); ~0 when none |
| Wakeups when idle | none: no timer, no thread activity |

Enforcement:

- **Nothing runs unless needed.** The ORT library is dlopened and the
  session created lazily, the first time an Auto binding comes into
  effect - not at daemon startup. The worker blocks on a condition
  variable while no Auto binding is in effect (no polling timer), and the
  RT tap flag is off; the worker thread itself only starts on first
  activation. The session is released after ~5 min with no Auto binding
  in effect, giving the memory back (the library stays mapped, see
  above).
- **Startup availability probe** (for the GUI's enabled/disabled state)
  only checks that the library (override, `LD_LIBRARY_PATH`, the usual
  directories, `ld.so.cache`) and model files exist; it doesn't load
  them. It can be re-run (installing ORT while the daemon runs enables
  Auto-detect), but a verdict from an actual load - Failed, too old,
  TooSlow - sticks until the device setting changes, so a broken install
  can't make Auto bindings flicker on and off.
- **Skip work:** no inference on silence (RMS gate) or when the winning
  Auto app isn't actually playing. The silence check itself (RMS of the
  ring, microseconds) runs every ~0.5 s so gaps between songs aren't
  missed between inference runs.
- **Adaptive cadence:** every ~2 s at the start of a spell, after a track
  boundary, or while a rival class is building up; every ~6 s once the
  class has been stable for the dwell time.
- **Duty-cycle governor** (pure, clock-injected, unit-tested): it tracks
  inference time (the worker thread's CPU time, so time spent preempted
  doesn't count against it) and stretches the interval so inference time / interval
  stays <= 1%. If even the longest interval (~15 s) can't meet it, the
  backend is marked "too slow on this machine", detection stops, and
  Auto bindings fall through like an unavailable runtime.
- **Lowest scheduling class:** the worker thread runs `SCHED_IDLE` (plus
  nice 19), so it only gets CPU nobody else wants. Under full load
  detection simply stalls and the last class is held - which is the
  right outcome.
- **ORT tuned for low overhead:** intra-op and inter-op threads = 1, no
  global thread pools, `session.intra_op.allow_spinning = 0` (ORT
  spin-waits by default, which burns idle CPU), CPU memory arena off
  (`DisableCpuMemArena`) so its size doesn't creep up, fixed input
  shape so buffers are allocated once.
- **Never block the audio or control paths:** model load, session
  creation and inference happen on the worker only; the PipeWire main
  loop and control threads only read the published class (an atomic /
  short mutex).
- **On battery** (`/sys/class/power_supply`), use the slow cadence
  from the start.
- **Accelerators earn their place:** an NPU/GPU provider is kept only if
  validation shows it faster than CPU (see above); GPU stays opt-in.

Visibility: the daemon tracks inference time (p50/p95), duty cycle and
the RSS added; `GetDetectedClasses` reports them, the GUI shows them in
the "Run detection on" tooltip ("NPU · 0.1% CPU · 4 ms/run"), and the
daemon logs a line when the governor backs off or gives up.

## Model

Candidate: **EfficientAT `mn10_as`** (MIT; ~5M params, ~20 MB fp32;
AudioSet-527 labels). Fallback: **YAMNet** (Apache-2.0; ~15 MB; 16 kHz
input). Both licenses are compatible with GPLv3 and the commercial
license. Phase 0 picks one on measured accuracy/size/CPU time. Shipped
in `data/models/` with its license, installed to
`share/ramkolfx/models` (like the HRTFs, behind an install option).

**Phase 0a export (mn10_as, done; tuning still open):**

- **Format:** 24 MB fp32 at opset 17. The input is a 5 s, 32 kHz mono waveform `[1, 160000]` (3 s until phase 9's sweep); the output is 527 sigmoid probabilities.
- **Front end:** EfficientAT's eval-mode mel front end runs in the graph as plain ops: pre-emphasis, a reflect-padded DFT as a strided conv, the Kaldi mel banks, log and `(x + 4.5) / 5`. It matches `AugmentMelSTFT` to 3e-5, and the scores match to 1e-6.
- **Minimum context:**
  - *Problem:* mn10 was trained on 10 s clips. Below ~400 mel frames (4 s) the zero-padded borders dominate its last feature map, and the head saturates: every logit goes positive, so random labels come out at 1.0.
  - *Fix:* the graph tiles a short window's frames to 500 (`ramkolfx.context_frames`). A tiled 3 s window agrees with 10 s predictions about as well as a real 5 s one, but on the phase 7 test set the real 5 s window scores better (see phase 9), so that's what ships now.
  - *Rule:* don't export a window that ends up under ~400 frames.
- **Cost on the dev machine (single thread):**
  - CPU per run: 10.5 ms (p95 10.8 ms).
  - Duty cycle: 0.53% at the 2 s cadence, 0.18% settled.
  - Session load: 43 ms.
  - RSS added: 49 MB.
  - Still to do: measure on the old reference machine.
- **Labels:** all 42 LabelMap names are present.
- **Genre scores are low** in absolute terms (AudioSet genre labels are sparse).
  - *Evidence:* on real music in 3 s windows, `Music` sits around 0.65–0.75, while even a clear genre peaks at ~0.3 and typically reads 0.1–0.2.
  - *Consequence:* the original absolute genre bar (0.35) was never reached, so Music always won.
  - *Rule now:* when it's music, the genre with the best track-long average (`GenreTauMs` 10 s, restarted at track boundaries) wins. There's no absolute bar, only a 0.01 floor. A rival genre must beat the current one by 1.25×. `Rapping` counts toward Hip-Hop.
  - *Measured limit:* on the user's music, mn10 keeps every genre at ≤ 0.11 even with 10–20 s windows (Music reaches 0.8–0.87). The pick is a weak signal: it's the best of small scores. A dedicated music-tagging model is the real fix.
  - *Known gap:* mn10 gave "Hip hop music" ≤ 0.05 on a 50 Cent track, which it heard as Pop/Dance/Electronic. Phase 0b decides whether that's this model's limit.

## Protocol changes

- New `BindAppAutoDetect` / `BindDeviceAutoDetect` (raw target name)
  bind a target to Auto-detect, replacing a preset binding. Separate
  opcodes rather than a flag on `BindAppPreset` / `BindDevicePreset`,
  whose payload ends in a raw preset name with nowhere to put one.
  `BindAppPreset` / `BindDevicePreset` with an empty preset unbind
  either kind.
- `ContentStreamListResponse` carries `bAutoDetect` as one flag byte per
  entry *after* the whole list, so older GUIs ignore it and a newer GUI
  still reads an older daemon's list (all false).
- Version skew: `Status` gains a decoder-only `bHasAutoDetect`; the GUI
  disables Auto-detect with "restart the daemon" against a daemon that
  predates it, and stops asking (instead of reconnecting) if
  `GetDetectedClasses` is refused - the same pattern as
  `GetChannelLevels`. Opcodes 0x1A-0x1E and response 0x89 sit in the
  range that commit reserved for this.
- New `BindDetectedClass([class][preset])` (empty preset = no switch;
  the default preset drops the override) and `GetDetectedClasses` ->
  `DetectedClassesResponse`: every class with its preset, its default preset
  and its live smoothed score, plus the perf counters
  (device label, p50/p95 CPU time per run, duty cycle, runs, RSS added).
  Asking also re-probes an unavailable runtime/model.
- New `SetAutoDetectDevice([InferenceDevice])`; re-sending the current
  device retries one that failed.
- New `SetAutoDetectTiming([checkEveryMs u32][switchAfterMs u32]
  [keepAtLeastMs u32][cpuLimitPercent float])` (0x1F), clamped by the
  daemon, applied live; `Status` appends the same block after
  `DetectedTableRevision` (decoder-only `bHasAutoDetectTiming`; the GUI
  disables the sliders against a daemon that predates it). Mapping:
  check every = fast cadence (slow = 3x); switch after = `ConfirmMs`,
  with the score EMA at 3/4 of it and the boundary window at it + 11 s;
  keep at least = `MinDwellMs`; CPU limit = the governor's duty cap
  (raising it forgets a TooSlow verdict). The defaults (2 s, 4 s, 10 s,
  1%) are the values above.
- `Status` appends: `AutoDetect` (ok / no runtime / no model / failed /
  too slow), `bDeviceAutoDetect`, `bAutoDetectActive` (an Auto binding
  is in effect; `PresetSource` stays App/Device and says which),
  `Detected` + confidence percent (None while listening),
  `bDetectionHeld`, `AutoDetectDevice` (the policy),
  `InferenceDeviceLabel` (where it actually runs) and
  `DetectedTableRevision` (bumped by `BindDetectedClass`; clients also
  re-fetch the table when `PresetCatalogRevision` changes). Decoded only
  if present, so a newer GUI still reads an older daemon's Status.

## Storage

- `presets.conf`: `app_auto.<n>=<app>`, `device_auto.<n>=<device>`
  (stored as sets on `EqPresetCatalog`, separate from the name-valued
  binding maps, so "every binding names a preset that exists" still
  holds) and `detected.<class>=<preset>`. Deleting a preset drops the
  class-table entries pointing at it (that class becomes "no switch");
  constructor renames of shadowed user presets apply to them too.
- `settings.conf`: `auto_detect.device` (`auto` | `cpu` | `gpu` | `npu`),
  `auto_detect.check_every_ms`, `auto_detect.switch_after_ms`,
  `auto_detect.keep_at_least_ms`, `auto_detect.cpu_limit_percent`.

## Phases

0. **Spike (outside the tree).** Export both candidate models to ONNX with
   in-graph preprocessing (fp32 + static int8). Run them on ~50 of the
   user's tracks plus podcasts and film clips; measure per-class
   accuracy, size, CPU latency (old and new machine), RSS of a loaded
   session, int8 agreement. Lock the `LabelMap`, thresholds, dwell time
   and the performance budget numbers. A model that can't meet the
   budget on the old machine is out, however accurate. Decides whether
   genres ship or v1 is Speech/Music/Movie only.
1. **Pure logic in `ramkolfx_presets`.** *(Done: `content_detection.*`,
   `ResolveBinding` + `AutoDetectSession` in `preset_auto_switch.*`,
   Auto targets and class table in `preset_store.*`; tuning values are
   placeholders until phase 0.)* `LabelMap`,
   `DetectionSmoother`, Auto-binding sets, class table, resolver's pending
   / detected targets, hold-on-tweak, duty-cycle governor and adaptive
   cadence. Unit tests only, no ORT.
2. **Inference plumbing.** *(Done, except measuring a real model: the
   ORT-path test and the bench's inference half need ONNX Runtime and
   phase 0's model. `content_detection.*` gained the downmix, `Rms`,
   `SilenceTracker`, `SampleHistory` and `Resampler`; `daemon/src/detect/`
   holds `ContentClassifier`, the ORT backend and the `ContentDetector`
   worker; AudioEngine taps into it but nothing activates it until
   phase 3. Measured: the RT tap costs ~11 ns per block when off and
   p99 < 0.15% of the block when on, on an i9-14900K.)* RT tap, resampler, worker, dlopen ORT backend,
   lazy load / idle unload, provider selection + validation + CPU
   fallback, availability state, perf counters. Parity test (golden WAV
   -> expected class) that runs only when ORT is present. A
   `ramkolfx-detect-bench` tool (not installed) that measures the RT tap
   cost per block, inference p50/p95, duty cycle and RSS against the
   budget table, run on the old and new reference machines before the
   phase is called done (CI timing is too noisy to gate on).
3. **Engine + protocol wiring.** *(Done. Every 250 ms tick, AudioEngine
   feeds the worker's events to its `AutoDetectSession`, resolves the
   binding with Auto-detect taking part while the detector is available,
   and switches the worker on/off (and settled) to match. Slider/bass
   changes and picking/saving a preset hold detection. An unavailable
   runtime/model is re-probed every ~30 s. Not yet run end to end: that
   needs phase 0's model (phase 5's e2e).)* New opcodes/fields,
   persistence, AudioEngine glue.
4. **GUI.** *(Done, not yet looked at on screen: "Auto-detect (AI)" in
   the app preset combo, an Auto-detect toggle next to the device line,
   "AI: Jazz 82%" after the active preset, and a collapsible Auto-detect
   card replacing the placeholder row - "Run detection on" with the live
   device / CPU share / ms per run, and the class -> preset table three
   classes per row with live score bars, fetched only while expanded.
   Unavailable states disable the Auto entries with the reason as a
   tooltip.)* "Auto-detect (AI)" entry in the app and device binding
   combos, live "auto: Jazz (82%)" readout, the class -> preset table
   (replacing the "Genre auto-detect" placeholder row), "Run detection
   on" dropdown, unavailable/warning states.
5. **Packaging + e2e.** *(Partly done.)*
   - *Done:* the model is installed to `share/ramkolfx/models`.
   - *ORT is a dependency:*
     - Arch: `onnxruntime`.
     - Debian 13: `libonnxruntime1.21`.
     - Fedora: `onnxruntime`.
     - Alpine (3.23+): `onnxruntime`.
     - Nix: the flake passes `-DRAMKOLFX_ORT_LIBRARY` with its store path.
     - Gentoo: manual install; ::gentoo has no ORT.
   - *Library lookup:* the loader also finds versioned sonames, such as
     Debian's `libonnxruntime.so.1.21`.
   - *To do:* an e2e test that runs `pw-play` speech and music into an
     Auto-bound app, then checks the switch and the restore.

The phases below come after the first release of Auto-detect, in this
order. Game detection comes first: it needs no license decisions and
fixes the most common case for this app. Model work waits for a test set
that can show whether a change helps.

6. **Game detection** from stream metadata. *(Done, not yet tried with
   a real game: the decision, resolver, session, wire and GUI are in, with
   unit tests on a faked `/proc`, plus one check against live processes.
   Still to confirm on real games: that SDL sets `media.role=Game`, and
   which binary names Proton reports.)* See
   [Game detection](#game-detection-phase-6).
7. **Test set and scoring.** About 100 of the user's tracks with known
   genres, plus podcasts, film clips and game recordings. A script runs
   a model over them and reports per-class accuracy and a confusion
   table. This is phase 0b; without it no model change can be judged.
   *(Tools done; the list is a starter one.)*
   - *The list:* [`tools/testset/clips.tsv`](../tools/testset/clips.tsv),
     102 clips: 7 per genre, 4 Music without a genre class, 8 Speech, 8
     Movie, 5 Game. Each line is a class, a name, a URL or search, and a
     section. It is a starter list of well-known material; replace it
     with what you listen to.
   - *Fetching:* `tools/testset/fetch.py` (yt-dlp + ffmpeg) downloads
     each section into `~/.local/share/ramkolfx/testset/<Class>/`,
     outside the repo since the clips are copyrighted. It keeps the
     downloaded stream as is (~1 MB a clip) and writes a
     `<name>.source.txt` naming the video a search landed on. Re-running
     fetches only new lines.
   - *Scoring:* `build/daemon/ramkolfx-detect-score` (built, not
     installed) replays each clip the way the daemon hears it: 48 kHz
     mono, 100 ms silence chunks, the worker's cadence (fast, then slow
     once settled), the same `LabelMap` and `DetectionSmoother`. The CPU
     governor is left out, so a busy machine doesn't change the result.
     - *Verdict:* the class held longest in the clip.
     - *Reported per class:* accuracy, the share of the clip's time on
       the right class, time to the first class, switches per clip, and
       a confusion table.
     - *Game clips:* shown but not scored, since games come from metadata.
   - *Comparing:* `--out run.tsv` saves the verdicts and the smoother's
     final evidence per clip. `--baseline run.tsv` on a later run lists
     the clips each change fixed or broke. `--model` scores another
     model, `--verbose` prints every inference.

   - *Baseline (mn10, starter list, 2026-11-20):* 64 of 97 scored clips
     right (66%).
     - *Perfect:* Speech, Hip-Hop, Jazz, Classical and Reggae, 7/7 or
       8/8 each. 50 Cent is Hip-Hop, so the phase 0a gap was that one
       window, not the model.
     - *Mostly right:* Rock, Metal, R&B and Acoustic, 5/7 each;
       Electronic 6/7.
     - *Weak:* Pop 1/7 (mostly heard as R&B) and Blues 1/7 (spread over
       Rock, R&B, Jazz and Acoustic). These are the genre model's limits
       (phase 9).
     - *Music 0/4:* a genre is always forced on music without one, by
       design (the 0.01 floor).
     - *Movie 0/8, a smoother problem rather than a model one:*
       - Film cues come in bursts (a gunshot window reads 0.4–0.5, the
         next 0.05). The 3 s EMA averages them below
         `MovieThreshold`, so dialogue scenes come out as Speech.
       - Action without dialogue or a clear score has neither Speech nor
         Music at `CoarseThreshold`, so nothing is detected (Mad Max,
         Inception).
       - *Fixed (5/8, nothing else changed; 69 of 97 overall):*
         - *Labels:* Movie now folds 36 sound-event labels (weapons,
           vehicles, impacts, weather, outdoor and large-room ambience,
           animals) by **sum**, capped at 1, not by max.
           "Soundtrack music" is out: it fired on songs more than on
           films.
         - *Averaging:* the evidence is a long weighted mean
           (`FilmTauMs` 20 s, restarted at track boundaries), which
           starts from two windows of no cues so one loud window can't
           decide it.
         - *Decision:* Movie is judged before the coarse threshold,
           at `MovieThreshold` 0.22.
         - *Per clip, on the test set:* action scenes average 0.2–0.46;
           songs peak at ~0.19 over 10 s, podcasts at ~0.11.
         - *Still missed:* the two dialogue-only scenes (Dark Knight, No
           Country) and Interstellar (0.14). A dialogue scene sounds
           like a podcast to mn10.
         - *Game recordings:* now Movie (4 of 5), a better fallback than
           Speech when metadata misses a game.
         - *Cost:* the first Movie lands ~30 s into a scene on average.
         - *Side effect:* nature or rain recordings may read as Movie.
     - *Game audio:* Speech, Electronic or nothing, never Movie;
       metadata has to carry games.

   ```sh
   tools/testset/fetch.py                       # pipx install yt-dlp first
   build/daemon/ramkolfx-detect-score --out baseline.tsv
   # change the model or the tuning, rebuild, then:
   build/daemon/ramkolfx-detect-score --baseline baseline.tsv
   ```
8. **Model plumbing.** Three changes, none of which needs a new model:
   - *Classes from metadata.* The model's metadata maps its labels to
     classes, instead of the AudioSet names hard-coded in `DefaultFold`.
     A model with non-AudioSet labels (`rock`, `jazz`) then loads; today
     it is refused for lacking Speech/Music labels.
   - *An optional second model.* A genre model that runs only on
     windows the first model hears as music.
   - *A reload command.* A replaced model file is picked up without a
     daemon restart. Today a new file is only read on the next load:
     after a restart, a "Run detection on" change, or the 5-minute idle
     unload.
9. **A better genre model.** It needs a permissive license, since
   non-commercial weights conflict with the commercial license. That
   rules out Essentia's Discogs and Jamendo models. The options are a
   small genre head trained on mn10's embeddings (the licenses of FMA,
   MTG-Jamendo and GTZAN need checking), or a bigger EfficientAT model
   (mn40/dymn, MIT; same weak AudioSet genre labels). Also: when the
   player reports a genre over MPRIS (`xesam:genre`; local players do,
   Spotify doesn't), trust it over the model.
   *Tried so far (2026-11-20):*
   - *Licence-clean data is scarce where it's needed.* Only ~7% of FMA
     allows commercial use (CC BY, CC0, Public Domain Mark; no NC, ND or
     ShareAlike). That leaves ~1,300 Rock and ~2,000 Electronic tracks,
     but only 71 R&B (13 artists), 49 Blues and 15 Reggae (3 artists).
     `tools/model/fma_select.py` picks them and `fma_fetch.py` reads
     single clips out of `fma_large.zip` over HTTP ranges.
   - *Per-genre weights on the AudioSet fold don't help.*
     `tools/model/calibrate_genres.py` fits one weight per genre on 636
     clean FMA tracks:
     - *Held out by artist:* mean accuracy goes from 56% to 54%.
     - *On the test set's genre clips:* 58 of 77 right before, 53 after.
       The fitted weights don't transfer: R&B's 0.2 loses all 7 R&B
       clips.
     - *Conclusion:* the genre levels aren't the problem; mn10's genre
       labels don't separate Pop, Blues and R&B, and FMA's indie catalogue
       differs from what people play. Not shipped.
   - *A head trained on the embedding loses.* `tools/model/genre_head.py`
     trains a softmax over mn10's 960-number embedding (11 genres plus
     "other") on the 755 clean FMA tracks:
     - *Unseen FMA artists:* 46%, below the AudioSet fold's ~56% on the
       same tracks.
     - *Test set's genre and Music clips:* 33 of 81, against the fold's
       58.
     - *Conclusion:* this little noisy, indie-tagged data teaches less
       than mn10 learned from AudioSet's ~2M clips.
   - *Bigger models and longer windows help.* The whole test set scored
     with `ramkolfx-detect-score`; CPU per run single-threaded on the
     dev machine (i9-14900K); RSS from `ramkolfx-detect-bench`:

     | Model, window | Right of 97 | ms/run | RSS added |
     |---|---|---|---|
     | mn10, 3 s (tiled to 5 s) | 69 | 10.6 | 49 MB |
     | **mn10, 5 s (shipped)** | **71** | 14.0 | 49 MB |
     | mn10, 10 s | 71 | 24.5 | |
     | mn20, 3 s | 74 | 21.8 | |
     | mn20, 5 s | 76 | 26.4 | 120 MB |
     | mn20, 10 s | 72 | 47 | |
     | mn30, 3 s | 70 | 40.4 | |
     | mn40, 3 s | 77 | 65 | |
     | mn40_ext, 3 s | 74 | 65 | |

     - *Noise:* the order isn't monotonic (mn30 below mn20), so a few
       clips either way is noise. On the FMA tracks every model scores
       53–57%.
     - *Why mn20 isn't shipped:* mn20 at 5 s is the best step up, but it
       breaks the budget: 1.3% of a core at the fast cadence (the
       governor would stretch it), 120 MB RSS against 60, and 73 MB on
       disk. mn40 costs 65 ms.
     - *int8 (static, QDQ, front end left in fp32) is unreliable:*
       - mn20 at 5 s: 77 right, 19.6 ms.
       - mn40: 59 right.
       - mn10 at 5 s: 66 right, and no faster.
     - *dymn models* (dymn20: mAP 0.493) don't export yet: an adaptive
       pooling size isn't constant at trace time.
     - *Speech pauses:* a 5 s window that had to be all sound held
       podcasts and audiobooks back by up to 30 s, since pauses between
       sentences break it. Now only the window's last 3 s must be sound
       (`ContentDetectorTuning::MinAudibleMs`).
   - *Still open:*
     - mn20 at 5 s, if the budget can grow.
     - A music-tagging model with a permissive licence.
     - More licence-clean training data (Jamendo's API needs a key).
10. **More genres** (Latin, Country split from Acoustic, Ambient/New-age,
    Soundtrack, World), chosen by what phase 9's model can actually tell
    apart. A genre only earns a class if its preset sounds different
    from the existing ones.
11. **(Optional) Online game-genre lookup:** `SteamAppId` -> Steam store
    genres -> a Games preset, cached locally. Opt-in, off by default:
    it breaks "everything runs locally". The official API has coarse
    genres only (Action, RPG, Strategy, Racing); FPS and Horror are user
    tags it doesn't expose.
