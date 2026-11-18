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

AutoSwitchTarget ResolveAutoSwitchTarget(const AppActivityTracker& Activity, const PresetBindingMap& AppBindings,
                                         const std::string& OutputDevice, const PresetBindingMap& DeviceBindings)
{
    const std::string* WinnerApp = nullptr;
    const std::string* WinnerPreset = nullptr;
    int64_t WinnerStartedAt = 0;
    // Activity.Apps() iterates in name order, so on equal start times the
    // alphabetically first app wins - strict '>' keeps the earlier one.
    for (const auto& [App, State] : Activity.Apps())
    {
        const auto Binding = AppBindings.find(App);
        if (Binding == AppBindings.end())
        {
            continue;
        }
        if (!WinnerApp || State.StartedAtMs > WinnerStartedAt)
        {
            WinnerApp = &App;
            WinnerPreset = &Binding->second;
            WinnerStartedAt = State.StartedAtMs;
        }
    }
    if (WinnerApp)
    {
        return {EqPresetSource::App, *WinnerApp, *WinnerPreset};
    }

    if (const auto Binding = DeviceBindings.find(OutputDevice);
        !OutputDevice.empty() && Binding != DeviceBindings.end())
    {
        return {EqPresetSource::Device, OutputDevice, Binding->second};
    }
    return {};
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
