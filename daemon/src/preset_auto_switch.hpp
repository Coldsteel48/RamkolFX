// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "content_detection.hpp"
#include "preset_store.hpp"

namespace ramkolfx
{

// Pure, clock-injected logic behind EQ preset auto-switching (see
// docs/eq-presets-plan.md) - no PipeWire, no locking, so it can be unit
// tested directly. AudioEngine feeds it the live stream list on a timer.

// How long an app counts as still playing after its last stream stopped
// running, so pausing a video or skipping a track doesn't flip the EQ
// back and forth.
inline constexpr int64_t AutoSwitchGraceMs = 2000;

// Tracks, per app name, when it started playing and when it was last
// seen playing.
class AppActivityTracker
{
public:
    struct Activity
    {
        int64_t StartedAtMs = 0;     // start of the current playing spell (survives gaps < grace)
        int64_t LastPlayingAtMs = 0; // last Update() at which any of its streams was running
    };

    // PlayingApps: every app with at least one running stream right now.
    // An app that (re)starts after being silent for longer than GraceMs
    // gets a fresh StartedAtMs; entries silent for longer than GraceMs are
    // dropped.
    void Update(const std::vector<std::string>& PlayingApps, int64_t NowMs, int64_t GraceMs = AutoSwitchGraceMs);

    const std::map<std::string, Activity>& Apps() const
    {
        return Activities;
    }

private:
    std::map<std::string, Activity> Activities;
};

struct AutoSwitchTarget
{
    EqPresetSource Source = EqPresetSource::Manual; // Manual = nothing bound applies
    std::string Key;                                // device node.name or app name; empty for Manual
    std::string PresetName;

    bool operator==(const AutoSwitchTarget&) const = default;
};

// The binding the resolution rule picked: a preset binding, or an
// Auto-detect one (whose preset comes from AutoDetectSession).
struct ResolvedBinding
{
    EqPresetSource Source = EqPresetSource::Manual; // Manual = nothing bound applies
    std::string Key;
    std::string PresetName;   // empty for Auto-detect
    bool bAutoDetect = false;
    int64_t SpellStartedAtMs = 0; // app bindings: start of the app's playing spell
    // Auto-detect only: a game is in effect - the bound app is one, or,
    // for a device binding, any app playing is. The class is then Game,
    // from metadata, and the audio model doesn't run. Part of the
    // binding's identity: a game starting or stopping starts afresh.
    bool bGame = false;

    bool operator==(const ResolvedBinding&) const = default;

    // As a switch target: Auto-detect resolves to the pending target
    // (empty preset - binding in effect, curve left alone).
    AutoSwitchTarget AsTarget() const
    {
        return {Source, Key, PresetName};
    }
};

// The resolution rule: among apps tracked by Activity (i.e. playing, or
// within the grace period) that have a binding, the one that started
// playing most recently wins (ties broken by name, for determinism);
// otherwise the current output device's binding; otherwise Manual.
// Auto-detect bindings take part only if bAutoDetectAvailable; otherwise
// they count as unbound and the rule falls through past them. GameApps:
// the apps whose streams look like games (game_detection.hpp), which set
// an Auto-detect binding's bGame.
ResolvedBinding ResolveBinding(const AppActivityTracker& Activity, const EqPresetCatalog& Catalog,
                               const std::string& OutputDevice, bool bAutoDetectAvailable,
                               const std::set<std::string>& GameApps = {});

// Runs detection for the Auto-detect binding in effect and turns its
// result into a switch target (docs/content-detect-plan.md). AudioEngine
// owns one, feeds it every resolved binding and every inference result,
// and tells it when the user takes over. Not thread-safe.
class AutoDetectSession
{
public:
    explicit AutoDetectSession(DetectionTuning Tuning = {}) : Smoother(Tuning)
    {
    }

    // New smoothing/confirm/dwell timings (see DetectionTuningFor), from
    // the next inference result on; what was detected so far stands.
    void SetTuning(const DetectionTuning& Tuning)
    {
        Smoother.SetTuning(Tuning);
    }

    // Binding is what the rule resolved this tick. Returns the target for
    // AutoSwitchController: a preset binding as-is; for Auto-detect, the
    // preset the class table gives the detected class - or the pending
    // target (empty preset) while nothing is detected yet, the class maps
    // to no switch, or detection is held. A different Auto-detect binding
    // (or the same app's new playing spell) starts detection afresh.
    AutoSwitchTarget Update(const ResolvedBinding& Binding, const EqPresetCatalog& Catalog);

    // An inference result, or a track boundary (see DetectionSmoother).
    // Ignored unless detection should run.
    void AddScores(const ClassScores& Scores, int64_t NowMs);
    void AddTrackBoundary(int64_t NowMs);

    // The user changed the EQ by hand (slider, picked or saved a preset)
    // at NowMs while an Auto-detect binding is in effect: leave their curve
    // alone until the content changes. The first gap of silence after
    // NowMs (a track boundary) starts a re-check: detection runs on what
    // plays next. If that maps to the same preset as what the user tuned
    // for - the same film resumed after a pause, a quiet passage, another
    // song of the same kind - the hold carries on until the next gap;
    // otherwise it ends and the new content's preset applies. If nothing
    // was classified yet when the user tuned, the class may be changing
    // (a gap just passed, a rival confirming), they re-tuned while held or
    // during a re-check (a gapless change goes unseen while held), or a
    // gap from before their last tune turns up late, detection first
    // learns what they tuned for. A new binding or spell
    // also ends the hold.
    void Hold(int64_t NowMs);

    // Who is in charge of the curve (see Hold):
    // - None: detection.
    // - Learning: the user; detection runs to find out what they tuned for.
    // - Held: the user; detection is paused, only gaps are watched for.
    // - Rechecking: the user; a gap since, detection runs to find out
    //   whether the content changed.
    enum class EHold
    {
        None,
        Learning,
        Held,
        Rechecking,
    };
    EHold HoldPhase() const
    {
        return bActive ? HoldState : EHold::None;
    }

    // An Auto-detect binding is in effect and inference is needed: not a
    // game, and not held, or held but learning / re-checking. (Otherwise,
    // while paused, the worker only tracks silence: see NeedsAudio.)
    bool ShouldDetect() const
    {
        return NeedsAudio() && HoldState != EHold::Held;
    }
    bool IsActive() const
    {
        return bActive;
    }
    // The worker should listen at all: an Auto-detect binding is in
    // effect and it isn't a game's, whose class comes from metadata.
    bool NeedsAudio() const
    {
        return bActive && !Binding.bGame;
    }
    // The user is in charge of the curve, whatever detection is doing.
    bool IsHeld() const
    {
        return bActive && HoldState != EHold::None;
    }
    // Held with detection paused, waiting for a gap.
    bool IsPaused() const
    {
        return bActive && HoldState == EHold::Held;
    }
    DetectedClass Current() const
    {
        if (!bActive)
        {
            return DetectedClass::None;
        }
        return Binding.bGame ? DetectedClass::Game : Smoother.Current();
    }
    float Confidence() const
    {
        if (!bActive)
        {
            return 0.0f;
        }
        return Binding.bGame ? 1.0f : Smoother.Confidence();
    }
    bool IsSettled(int64_t NowMs) const
    {
        return bActive && (Binding.bGame || Smoother.IsSettled(NowMs));
    }
    // The live per-class scores, for the class table: the smoother's, or
    // Game alone at 1 while a game is in effect. All 0 when not active.
    ClassScores LiveScores() const;
    const DetectionSmoother& GetSmoother() const
    {
        return Smoother;
    }

private:
    void EndHold();
    void StartRecheck();

    ResolvedBinding Binding;
    bool bActive = false;
    EHold HoldState = EHold::None;
    int64_t HeldAtMs = 0;                          // when the user last tuned; unless None
    DetectedClass HeldClass = DetectedClass::None; // what they tuned for; when Held or Rechecking
    DetectionSmoother Smoother;
};

// The live EQ as the switching logic sees it: the curve plus which preset
// it is (if any) and whether it has unsaved tweaks.
struct EqState
{
    std::array<EqBand, MaxEqBands> Bands{};
    BassEnhancerSettings Bass;
    std::string PresetName;
    bool bModified = false;
};

// The edge-triggered switching state machine (docs/eq-presets-plan.md),
// kept free of locking and DSP so it can be unit tested: AudioEngine owns
// one, feeds it the live EqState, and applies whatever it hands back.
//
// - A binding only applies when the resolved target *changes*.
// - Going from "no binding applies" to any binding snapshots the live EQ;
//   going back restores it.
// - Picking or saving a preset by hand while a binding is in effect makes
//   that the new baseline - unless it's the binding's own preset (reverting
//   or updating it), which leaves the binding in charge. Under an
//   Auto-detect binding, which can take back over once the content
//   changes, the old baseline returns if it does.
// - Explicitly unbinding the binding in effect keeps the current sound.
// - The same binding going pending (an Auto-detect binding whose
//   detection is held, or whose class maps to no switch) changes nothing:
//   neither the curve nor who is in charge of it.
class AutoSwitchController
{
public:
    // Applies Target if it differs from the last one. May rewrite Live
    // (curve and name); sets bOutCurveChanged when the curve itself must
    // be pushed to the DSP. Returns whether the target changed.
    bool Evaluate(const AutoSwitchTarget& Target, const EqPresetCatalog& Catalog, EqState& Live,
                  bool& bOutCurveChanged);

    // A preset was just picked or saved by hand; Live is the EQ after it.
    // bBindingMayResume: the binding in effect may take back over (an
    // Auto-detect binding, held only until the content changes). The pick
    // becomes the baseline as usual, but if a binding does switch the
    // curve again, the pre-binding baseline is restored as the one to
    // return to.
    void OnPresetChosenByHand(const EqState& Live, bool bBindingMayResume = false);

    // The user took over (e.g. deleted the active preset): manual source,
    // and if a binding is in effect, Live becomes its baseline.
    void TakeManualControl(const EqState& Live);

    // Binding (Source, Key) was just removed. If it's the one in effect,
    // Resolved (what the rule gives now) is adopted without applying its
    // preset, and Live becomes the baseline.
    void OnBindingRemoved(EqPresetSource Source, const std::string& Key, const AutoSwitchTarget& Resolved,
                          const EqState& Live);

    const AutoSwitchTarget& LastTarget() const
    {
        return Last;
    }
    // Why the active preset is active (Status::PresetSource / PresetSourceApp).
    EqPresetSource Source() const
    {
        return ActiveSource;
    }
    const std::string& SourceApp() const
    {
        return ActiveSourceApp;
    }
    bool HasSnapshot() const
    {
        return Snapshot.has_value();
    }

private:
    void SetSourceFromTarget(const AutoSwitchTarget& Target);
    // Manual source, and Live as the baseline if a binding is in effect.
    void AdoptAsBaseline(const EqState& Live);

    AutoSwitchTarget Last;
    std::optional<EqState> Snapshot;
    // The baseline a hand pick under an Auto-detect binding replaced: back
    // in Snapshot once a binding switches the curve again.
    std::optional<EqState> ResumeBaseline;
    EqPresetSource ActiveSource = EqPresetSource::Manual;
    std::string ActiveSourceApp;
};

} // namespace ramkolfx
