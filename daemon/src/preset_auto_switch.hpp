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
// they count as unbound and the rule falls through past them.
ResolvedBinding ResolveBinding(const AppActivityTracker& Activity, const EqPresetCatalog& Catalog,
                               const std::string& OutputDevice, bool bAutoDetectAvailable);

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
    // while an Auto-detect binding is in effect: leave the curve alone for
    // the rest of this binding's spell.
    void Hold();

    // An Auto-detect binding is in effect and not held - the inference
    // worker should run.
    bool ShouldDetect() const
    {
        return bActive && !bHeld;
    }
    bool IsActive() const
    {
        return bActive;
    }
    bool IsHeld() const
    {
        return bActive && bHeld;
    }
    DetectedClass Current() const
    {
        return bActive ? Smoother.Current() : DetectedClass::None;
    }
    float Confidence() const
    {
        return bActive ? Smoother.Confidence() : 0.0f;
    }
    bool IsSettled(int64_t NowMs) const
    {
        return bActive && Smoother.IsSettled(NowMs);
    }
    const DetectionSmoother& GetSmoother() const
    {
        return Smoother;
    }

private:
    ResolvedBinding Binding;
    bool bActive = false;
    bool bHeld = false;
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
//   or updating it), which leaves the binding in charge.
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
    void OnPresetChosenByHand(const EqState& Live);

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

    AutoSwitchTarget Last;
    std::optional<EqState> Snapshot;
    EqPresetSource ActiveSource = EqPresetSource::Manual;
    std::string ActiveSourceApp;
};

} // namespace ramkolfx
