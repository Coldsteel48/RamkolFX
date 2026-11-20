// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "preset_auto_switch.hpp"

#include <algorithm>

namespace ramkolfx
{

void AppActivityTracker::Update(const std::vector<std::string>& PlayingApps, int64_t NowMs, int64_t GraceMs)
{
    for (const std::string& App : PlayingApps)
    {
        auto [It, bInserted] = Activities.try_emplace(App);
        if (bInserted)
        {
            It->second.StartedAtMs = NowMs;
        }
        It->second.LastPlayingAtMs = NowMs;
    }
    std::erase_if(Activities,
                  [&](const auto& Entry) { return NowMs - Entry.second.LastPlayingAtMs > GraceMs; });
}

ResolvedBinding ResolveBinding(const AppActivityTracker& Activity, const EqPresetCatalog& Catalog,
                               const std::string& OutputDevice, bool bAutoDetectAvailable)
{
    const PresetBindingMap& AppBindings = Catalog.AppBindings();
    const std::string* WinnerApp = nullptr;
    const AppActivityTracker::Activity* WinnerState = nullptr;
    // Activity.Apps() iterates in name order, so on equal start times the
    // alphabetically first app wins - strict '>' keeps the earlier one.
    for (const auto& [App, State] : Activity.Apps())
    {
        const bool bBound = AppBindings.contains(App) || (bAutoDetectAvailable && Catalog.IsAppAutoDetect(App));
        if (bBound && (!WinnerApp || State.StartedAtMs > WinnerState->StartedAtMs))
        {
            WinnerApp = &App;
            WinnerState = &State;
        }
    }
    if (WinnerApp)
    {
        const std::string Preset = Catalog.AppBinding(*WinnerApp);
        return {EqPresetSource::App, *WinnerApp, Preset, Preset.empty(), WinnerState->StartedAtMs};
    }

    if (OutputDevice.empty())
    {
        return {};
    }
    if (std::string Preset = Catalog.DeviceBinding(OutputDevice); !Preset.empty())
    {
        return {EqPresetSource::Device, OutputDevice, std::move(Preset), false, 0};
    }
    if (bAutoDetectAvailable && Catalog.IsDeviceAutoDetect(OutputDevice))
    {
        return {EqPresetSource::Device, OutputDevice, "", true, 0};
    }
    return {};
}

AutoSwitchTarget AutoDetectSession::Update(const ResolvedBinding& NewBinding, const EqPresetCatalog& Catalog)
{
    if (!NewBinding.bAutoDetect)
    {
        bActive = false;
        return NewBinding.AsTarget();
    }
    if (!bActive || !(NewBinding == Binding))
    {
        Binding = NewBinding;
        bActive = true;
        bHeld = false;
        Smoother.Reset();
    }
    AutoSwitchTarget Target = Binding.AsTarget();
    if (!bHeld)
    {
        Target.PresetName = Catalog.DetectedClassPreset(Smoother.Current());
    }
    return Target;
}

void AutoDetectSession::AddScores(const ClassScores& Scores, int64_t NowMs)
{
    if (ShouldDetect())
    {
        Smoother.AddScores(Scores, NowMs);
    }
}

void AutoDetectSession::AddTrackBoundary(int64_t NowMs)
{
    if (ShouldDetect())
    {
        Smoother.AddTrackBoundary(NowMs);
    }
}

void AutoDetectSession::Hold()
{
    bHeld = bActive;
}

void AutoSwitchController::SetSourceFromTarget(const AutoSwitchTarget& Target)
{
    ActiveSource = Target.Source;
    ActiveSourceApp = Target.Source == EqPresetSource::App ? Target.Key : std::string();
}

bool AutoSwitchController::Evaluate(const AutoSwitchTarget& Target, const EqPresetCatalog& Catalog, EqState& Live,
                                    bool& bOutCurveChanged)
{
    bOutCurveChanged = false;
    if (Target == Last)
    {
        return false;
    }
    if (Target.PresetName.empty() && Target.Source != EqPresetSource::Manual && Target.Source == Last.Source &&
        Target.Key == Last.Key)
    {
        // The binding in effect went pending: keep the curve, and keep
        // whoever is in charge of it (the user, if they took over).
        Last = Target;
        return true;
    }

    if (Target.Source == EqPresetSource::Manual)
    {
        // Back to "nothing bound applies": restore what was in effect
        // before the first binding kicked in, if anything - minus a preset
        // name deleted in the meantime.
        if (Snapshot)
        {
            const bool bNameStillExists = Catalog.Find(Snapshot->PresetName) != nullptr;
            Live = std::move(*Snapshot);
            if (!bNameStillExists)
            {
                Live.PresetName.clear();
                Live.bModified = false;
            }
            Snapshot.reset();
            bOutCurveChanged = true;
        }
    }
    else
    {
        if (Last.Source == EqPresetSource::Manual)
        {
            Snapshot = Live;
        }
        // Re-applying the preset that's already active would throw away
        // unsaved tweaks to it (e.g. binding the device to the preset
        // being adjusted), so only switch on a real change.
        const EqPreset* Preset = Catalog.Find(Target.PresetName);
        if (Preset && Live.PresetName != Preset->Name)
        {
            Live.Bands = Preset->Bands;
            Live.Bass = Preset->Bass;
            Live.PresetName = Preset->Name;
            Live.bModified = false;
            bOutCurveChanged = true;
        }
    }
    SetSourceFromTarget(Target);
    Last = Target;
    return true;
}

void AutoSwitchController::OnPresetChosenByHand(const EqState& Live)
{
    // Re-picking (to revert tweaks) or saving over the preset the binding
    // in effect points to is maintenance of that binding's preset, not a
    // new baseline: keep the binding in charge and the pre-binding
    // snapshot intact, so the sound still returns to it afterwards.
    if (Last.Source != EqPresetSource::Manual && Live.PresetName == Last.PresetName)
    {
        SetSourceFromTarget(Last);
        return;
    }
    TakeManualControl(Live);
}

void AutoSwitchController::TakeManualControl(const EqState& Live)
{
    // Re-captured rather than dropped: with a device binding still
    // underneath, there'd be no baseline left to return to once the device
    // changes. Last stays as-is, so the same binding isn't re-applied on
    // the next evaluation - only a change of target switches again.
    ActiveSource = EqPresetSource::Manual;
    ActiveSourceApp.clear();
    if (Last.Source != EqPresetSource::Manual)
    {
        Snapshot = Live;
    }
    else
    {
        Snapshot.reset();
    }
}

void AutoSwitchController::OnBindingRemoved(EqPresetSource Source, const std::string& Key,
                                            const AutoSwitchTarget& Resolved, const EqState& Live)
{
    // Turning the binding in effect off keeps today's sound rather than
    // restoring what was there before it applied - the user is switching
    // it off, not asking to undo it.
    if (Last.Source == Source && Last.Key == Key)
    {
        Last = Resolved;
        TakeManualControl(Live);
    }
}

} // namespace ramkolfx
