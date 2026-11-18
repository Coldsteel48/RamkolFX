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

// The resolution rule: among apps tracked by Activity (i.e. playing, or
// within the grace period) that have a binding, the one that started
// playing most recently wins (ties broken by name, for determinism);
// otherwise the current output device's binding; otherwise Manual.
AutoSwitchTarget ResolveAutoSwitchTarget(const AppActivityTracker& Activity, const PresetBindingMap& AppBindings,
                                         const std::string& OutputDevice, const PresetBindingMap& DeviceBindings);

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
