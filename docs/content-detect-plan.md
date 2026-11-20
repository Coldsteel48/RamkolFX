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
  phase 0.
- **Tweaks pause detection.** Moving a slider or picking/saving a preset
  by hand while an Auto binding is in effect holds detection for the rest
  of that binding's spell: the resolver returns the pending target
  `{Source, Key, ""}`, which leaves the curve as the user set it.
  Detection resumes the next time the binding comes into effect.
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
| Movie    | Film cues (Explosion, Gunshot, Vehicle, Soundtrack music) over Speech or Music | Movie   |
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

- The class table is **global** (shared by every Auto binding) and
  user-editable: any class can point at any preset, user presets
  included, or at nothing ("no switch").
- No new built-in presets: the General/Music built-ins already cover
  every class.
- Games are not a v1 class: from audio alone a game is hard to tell from
  a film. Follow-up (phase 6): stream metadata (`media.role=Game`, or a
  wine/proton/steam process) as a cheap, accurate non-ML signal.

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

- **Format:** 24 MB fp32 at opset 17. The input is a 3 s, 32 kHz mono waveform `[1, 96000]`; the output is 527 sigmoid probabilities.
- **Front end:** EfficientAT's eval-mode mel front end runs in the graph as plain ops: pre-emphasis, a reflect-padded DFT as a strided conv, the Kaldi mel banks, log and `(x + 4.5) / 5`. It matches `AugmentMelSTFT` to 3e-5, and the scores match to 1e-6.
- **Minimum context:**
  - *Problem:* mn10 was trained on 10 s clips. Below ~400 mel frames (4 s) the zero-padded borders dominate its last feature map, and the head saturates: every logit goes positive, so random labels come out at 1.0.
  - *Fix:* the graph tiles the window's 300 frames to 500 (`ramkolfx.context_frames`). That agrees with 10 s predictions about as well as a real 5 s window, costs the same, and answers after 3 s of sound instead of 5 s.
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
- `settings.conf`: `auto_detect.device` (`auto` | `cpu` | `gpu` | `npu`).

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
6. **(Follow-up) Game detection** from stream metadata.
