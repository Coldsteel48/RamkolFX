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

void AppActivityTracker::Update(const std::vector<std::string>& PlayingApps, int64_t NowMs,
                                const std::set<std::string>& GameApps, int64_t GraceMs)
{
    for (const std::string& App : PlayingApps)
    {
        auto [It, bInserted] = Activities.try_emplace(App);
        if (bInserted)
        {
            It->second.StartedAtMs = NowMs;
        }
        It->second.LastPlayingAtMs = NowMs;
        It->second.bGame = GameApps.contains(App);
    }
    std::erase_if(Activities,
                  [&](const auto& Entry) { return NowMs - Entry.second.LastPlayingAtMs > GraceMs; });
}

namespace
{
// Lower ranks first: Priority's order between games and everything else.
int KindRank(PlaybackPriority Priority, bool bGame)
{
    switch (Priority)
    {
    case PlaybackPriority::GameFirst:
        return bGame ? 0 : 1;
    case PlaybackPriority::MusicFirst:
        return bGame ? 1 : 0;
    default:
        return 0;
    }
}
} // namespace

ResolvedBinding ResolveBinding(const AppActivityTracker& Activity, const EqPresetCatalog& Catalog,
                               const std::string& OutputDevice, bool bAutoDetectAvailable,
                               PlaybackPriority Priority, BindingContest* OutContest)
{
    const PresetBindingMap& AppBindings = Catalog.AppBindings();
    const std::string DevicePreset = OutputDevice.empty() ? std::string() : Catalog.DeviceBinding(OutputDevice);
    const bool bDeviceAuto = !OutputDevice.empty() && DevicePreset.empty() && bAutoDetectAvailable &&
                             Catalog.IsDeviceAutoDetect(OutputDevice);

    const std::string* WinnerApp = nullptr;
    const AppActivityTracker::Activity* WinnerState = nullptr;
    bool bWinnerBound = false;
    int WinnerRank = 0;
    std::vector<const std::string*> Contenders;
    // Activity.Apps() iterates in name order, so on equal rank and start
    // time the alphabetically first app wins - strict '>' keeps the
    // earlier one.
    for (const auto& [App, State] : Activity.Apps())
    {
        const bool bBound = AppBindings.contains(App) || (bAutoDetectAvailable && Catalog.IsAppAutoDetect(App));
        const bool bGame = State.bGame;
        // If you're gaming under a device Auto binding, the game takes part
        // even unbound: it stands for the device binding.
        if (!bBound && !(bGame && bDeviceAuto))
        {
            continue;
        }
        Contenders.push_back(&App);
        const int Rank = KindRank(Priority, bGame);
        if (!WinnerApp || Rank < WinnerRank || (Rank == WinnerRank && State.StartedAtMs > WinnerState->StartedAtMs))
        {
            WinnerApp = &App;
            WinnerState = &State;
            bWinnerBound = bBound;
            WinnerRank = Rank;
        }
    }
    if (OutContest)
    {
        *OutContest = {};
        if (WinnerApp)
        {
            OutContest->WinnerApp = *WinnerApp;
            OutContest->bWinnerIsGame = WinnerState->bGame;
            for (const std::string* App : Contenders)
            {
                if (App != WinnerApp)
                {
                    OutContest->Waiting.push_back(*App);
                }
            }
        }
    }
    if (WinnerApp && bWinnerBound)
    {
        const std::string Preset = Catalog.AppBinding(*WinnerApp);
        const bool bAuto = Preset.empty();
        return {EqPresetSource::App, *WinnerApp, Preset, bAuto, WinnerState->StartedAtMs, bAuto && WinnerState->bGame};
    }
    if (WinnerApp)
    {
        // An unbound game under the device's Auto binding. No spell start:
        // a game reopening its stream mustn't look like a new binding.
        return {EqPresetSource::Device, OutputDevice, "", true, 0, true};
    }

    if (!DevicePreset.empty())
    {
        return {EqPresetSource::Device, OutputDevice, DevicePreset, false, 0};
    }
    if (bDeviceAuto)
    {
        // No game is playing: a game would have been a contender.
        return {EqPresetSource::Device, OutputDevice, "", true, 0, false};
    }
    return {};
}

AutoSwitchTarget AutoDetectSession::Update(const ResolvedBinding& NewBinding, const EqPresetCatalog& Catalog)
{
    if (!NewBinding.bAutoDetect)
    {
        bActive = false;
        EndHold();
        return NewBinding.AsTarget();
    }
    if (!bActive || !(NewBinding == Binding))
    {
        Binding = NewBinding;
        bActive = true;
        EndHold();
        Smoother.Reset();
    }
    if (Binding.bGame)
    {
        // The class is known from metadata; a hold lasts until the game
        // stops (the binding changes).
        AutoSwitchTarget Target = Binding.AsTarget();
        if (HoldState == EHold::None)
        {
            Target.PresetName = Catalog.DetectedClassPreset(DetectedClass::Game);
        }
        return Target;
    }
    if (Smoother.Current() != DetectedClass::None)
    {
        if (HoldState == EHold::Learning)
        {
            HeldClass = Smoother.Current();
            HoldState = EHold::Held;
        }
        else if (HoldState == EHold::Rechecking)
        {
            // Compared by preset: the user tuned for what the class table
            // gives the held content, and a class that maps to the same
            // preset wouldn't have switched.
            if (Catalog.DetectedClassPreset(Smoother.Current()) == Catalog.DetectedClassPreset(HeldClass))
            {
                HoldState = EHold::Held; // same kind of content: the user's curve still fits
            }
            else
            {
                EndHold();
            }
        }
    }
    AutoSwitchTarget Target = Binding.AsTarget();
    if (HoldState == EHold::None)
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

ClassScores AutoDetectSession::LiveScores() const
{
    if (!bActive)
    {
        return {};
    }
    if (Binding.bGame)
    {
        ClassScores Scores{};
        ScoreOf(Scores, DetectedClass::Game) = 1.0f;
        return Scores;
    }
    return Smoother.Smoothed();
}

void AutoDetectSession::AddTrackBoundary(int64_t NowMs)
{
    if (!NeedsAudio())
    {
        return;
    }
    // Under a hold, a gap is placed against when the user last tuned:
    // before it, they tuned for what followed the gap (the event can be
    // collected after the hold, up to a poll interval and a tick late);
    // after it, the content may have changed since.
    switch (HoldState)
    {
    case EHold::None:
    case EHold::Rechecking:
        Smoother.AddTrackBoundary(NowMs);
        break;
    case EHold::Learning:
        if (NowMs <= HeldAtMs)
        {
            Smoother.Reset(); // learn what started there, nothing from before
        }
        else
        {
            // Over before detection confirmed what it was: what the
            // scores pointed at stands in for it.
            const DetectedClass Tuned =
                Smoother.Current() != DetectedClass::None ? Smoother.Current() : Smoother.Decide();
            if (Tuned == DetectedClass::None)
            {
                EndHold(); // nothing to compare the next content with
                Smoother.AddTrackBoundary(NowMs);
            }
            else
            {
                HeldClass = Tuned;
                StartRecheck();
            }
        }
        break;
    case EHold::Held:
        if (NowMs <= HeldAtMs)
        {
            HeldClass = DetectedClass::None;
            HoldState = EHold::Learning;
            Smoother.Reset();
        }
        else
        {
            StartRecheck();
        }
        break;
    }
}

void AutoDetectSession::Hold(int64_t NowMs)
{
    if (!bActive)
    {
        return;
    }
    if (Binding.bGame)
    {
        // Nothing to learn or re-check: held until the game stops.
        HeldClass = DetectedClass::Game;
        HoldState = EHold::Held;
        HeldAtMs = NowMs;
        return;
    }
    switch (HoldState)
    {
    case EHold::None:
        // What the user is hearing is what detection last confirmed -
        // unless nothing is yet, or it may be on its way out (a gap just
        // passed, a rival class confirming): then learn it afresh.
        if (Smoother.MayBeChanging(NowMs))
        {
            Smoother.Reset();
        }
        HeldClass = Smoother.Current();
        HoldState = HeldClass == DetectedClass::None ? EHold::Learning : EHold::Held;
        break;
    case EHold::Rechecking:
        // Re-tuned for content not classified yet: learn it (the smoother
        // has been on it since the gap).
        HeldClass = DetectedClass::None;
        HoldState = EHold::Learning;
        break;
    case EHold::Held:
        // Detection was paused, so a gapless change (crossfade, autoplay
        // with no silence) went unseen: re-tuning may be for content other
        // than HeldClass. Learn what's playing now.
        HeldClass = DetectedClass::None;
        HoldState = EHold::Learning;
        Smoother.Reset();
        break;
    case EHold::Learning:
        break; // already learning what's playing
    }
    HeldAtMs = NowMs;
}

void AutoDetectSession::StartRecheck()
{
    HoldState = EHold::Rechecking;
    Smoother.Reset(); // judge what plays next on its own
}

void AutoDetectSession::EndHold()
{
    HoldState = EHold::None;
    HeldAtMs = 0;
    HeldClass = DetectedClass::None;
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
        ResumeBaseline.reset();
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
            if (ResumeBaseline)
            {
                // A binding takes back over from the user's temporary
                // pick: the sound to return to is again the one from
                // before any binding. (Only on a switch: a binding that
                // leaves the pick playing keeps it as the baseline.)
                Snapshot = std::move(*ResumeBaseline);
                ResumeBaseline.reset();
            }
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

void AutoSwitchController::OnPresetChosenByHand(const EqState& Live, bool bBindingMayResume)
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
    if (bBindingMayResume && Last.Source != EqPresetSource::Manual)
    {
        if (!ResumeBaseline)
        {
            ResumeBaseline = Snapshot; // kept from a run's first pick: later ones replace the pick
        }
    }
    else
    {
        ResumeBaseline.reset();
    }
    AdoptAsBaseline(Live);
}

void AutoSwitchController::TakeManualControl(const EqState& Live)
{
    ResumeBaseline.reset(); // an explicit takeover: no binding is coming back
    AdoptAsBaseline(Live);
}

void AutoSwitchController::AdoptAsBaseline(const EqState& Live)
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
