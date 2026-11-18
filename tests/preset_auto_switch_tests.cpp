// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// The EQ preset auto-switching rule (docs/eq-presets-plan.md): which
// binding wins, and the grace period around apps pausing.

#include "dsp/graphic_eq_filter.hpp"
#include "preset_auto_switch.hpp"
#include "test_harness.hpp"

using namespace ramkolfx;

namespace
{
const PresetBindingMap AppBindings{{"Firefox", "Vocal"}, {"mpv", "Bass Boost"}};
const PresetBindingMap DeviceBindings{{"headphones", "Flat"}};

AutoSwitchTarget Resolve(const AppActivityTracker& Tracker, const std::string& Device)
{
    return ResolveAutoSwitchTarget(Tracker, AppBindings, Device, DeviceBindings);
}
} // namespace

TEST(NothingPlayingFallsBackToDeviceThenManual)
{
    AppActivityTracker Tracker;
    Tracker.Update({}, 0);
    CHECK((Resolve(Tracker, "headphones") == AutoSwitchTarget{EqPresetSource::Device, "headphones", "Flat"}));
    CHECK(Resolve(Tracker, "speakers") == AutoSwitchTarget{});
    CHECK(Resolve(Tracker, "") == AutoSwitchTarget{});
}

TEST(UnboundAppIsIgnored)
{
    AppActivityTracker Tracker;
    Tracker.Update({"Spotify"}, 0);
    CHECK(Resolve(Tracker, "speakers") == AutoSwitchTarget{});
    CHECK(Resolve(Tracker, "headphones").Source == EqPresetSource::Device);
}

TEST(BoundAppBeatsDevice)
{
    AppActivityTracker Tracker;
    Tracker.Update({"Firefox"}, 0);
    CHECK((Resolve(Tracker, "headphones") == AutoSwitchTarget{EqPresetSource::App, "Firefox", "Vocal"}));
}

TEST(MostRecentlyStartedAppWins)
{
    AppActivityTracker Tracker;
    Tracker.Update({"Firefox"}, 1000);
    Tracker.Update({"Firefox", "mpv"}, 1500);
    CHECK(Resolve(Tracker, "").Key == "mpv");
    // Firefox keeps playing; mpv's later start still wins.
    Tracker.Update({"Firefox", "mpv"}, 5000);
    CHECK(Resolve(Tracker, "").Key == "mpv");
}

TEST(GracePeriodKeepsPausedAppActive)
{
    AppActivityTracker Tracker;
    Tracker.Update({"Firefox"}, 1000);
    Tracker.Update({"Firefox", "mpv"}, 1500);
    Tracker.Update({"Firefox"}, 3400); // mpv silent for 1900ms: within grace
    CHECK(Resolve(Tracker, "").Key == "mpv");
    Tracker.Update({"Firefox"}, 3600); // 2100ms: dropped
    CHECK(Resolve(Tracker, "").Key == "Firefox");
}

TEST(ResumeWithinGraceKeepsStartTime)
{
    AppActivityTracker Tracker;
    Tracker.Update({"mpv"}, 0);
    Tracker.Update({"Firefox"}, 500);
    Tracker.Update({"mpv"}, 1500); // mpv resumes after 1000ms gap: same spell, started at 0
    CHECK(Tracker.Apps().at("mpv").StartedAtMs == 0);
    CHECK(Resolve(Tracker, "").Key == "Firefox"); // Firefox (500) started later than mpv (0)
}

TEST(RestartAfterGraceGetsFreshStartTime)
{
    AppActivityTracker Tracker;
    Tracker.Update({"mpv"}, 0);
    Tracker.Update({"Firefox"}, 500);
    Tracker.Update({"Firefox"}, 2500); // mpv dropped
    Tracker.Update({"Firefox", "mpv"}, 2600);
    CHECK(Tracker.Apps().at("mpv").StartedAtMs == 2600);
    CHECK(Resolve(Tracker, "").Key == "mpv");
}

TEST(EverythingStopsReturnsToDevice)
{
    AppActivityTracker Tracker;
    Tracker.Update({"Firefox"}, 0);
    Tracker.Update({}, AutoSwitchGraceMs + 1);
    CHECK(Tracker.Apps().empty());
    CHECK(Resolve(Tracker, "headphones").Source == EqPresetSource::Device);
}

TEST(TieBrokenByName)
{
    AppActivityTracker Tracker;
    Tracker.Update({"mpv", "Firefox"}, 0);
    CHECK(Resolve(Tracker, "").Key == "Firefox");
}

// --- AutoSwitchController: the switching state machine ---------------------

namespace
{
const AutoSwitchTarget Manual{};
const AutoSwitchTarget FirefoxVocal{EqPresetSource::App, "Firefox", "Vocal"};
const AutoSwitchTarget HeadphonesBass{EqPresetSource::Device, "headphones", "Bass Boost"};

// A hand-tuned, unnamed curve - what the user had before any binding.
EqState HandTuned()
{
    EqState State;
    State.Bands = DefaultHwEqBands();
    State.Bands[3].GainDb = -4.0f;
    return State;
}

// Simulates the engine picking a preset by hand: apply it, then tell the
// controller.
void PickByHand(AutoSwitchController& Controller, const EqPresetCatalog& Catalog, EqState& Live,
                const std::string& Name)
{
    const EqPreset* Preset = Catalog.Find(Name);
    REQUIRE(Preset);
    Live.Bands = Preset->Bands;
    Live.Bass = Preset->Bass;
    Live.PresetName = Name;
    Live.bModified = false;
    Controller.OnPresetChosenByHand(Live);
}

bool Evaluate(AutoSwitchController& Controller, const EqPresetCatalog& Catalog, const AutoSwitchTarget& Target,
              EqState& Live)
{
    bool bCurveChanged = false;
    Controller.Evaluate(Target, Catalog, Live, bCurveChanged);
    return bCurveChanged;
}
} // namespace

TEST(BindingAppliesAndRestoresTheBaseline)
{
    const EqPresetCatalog Catalog;
    AutoSwitchController Controller;
    EqState Live = HandTuned();

    CHECK(Evaluate(Controller, Catalog, FirefoxVocal, Live));
    CHECK(Live.PresetName == "Vocal");
    CHECK(Controller.Source() == EqPresetSource::App);
    CHECK(Controller.SourceApp() == "Firefox");

    CHECK(Evaluate(Controller, Catalog, Manual, Live));
    CHECK(Live.Bands[3].GainDb == -4.0f);
    CHECK(Live.PresetName.empty());
    CHECK(Controller.Source() == EqPresetSource::Manual);
}

TEST(SameTargetIsNotReapplied)
{
    const EqPresetCatalog Catalog;
    AutoSwitchController Controller;
    EqState Live = HandTuned();
    Evaluate(Controller, Catalog, FirefoxVocal, Live);

    Live.Bands[0].GainDb = 9.0f; // tweak while the binding is in effect
    Live.bModified = true;
    bool bCurveChanged = true;
    CHECK(!Controller.Evaluate(FirefoxVocal, Catalog, Live, bCurveChanged));
    CHECK(!bCurveChanged);
    CHECK(Live.Bands[0].GainDb == 9.0f);
}

TEST(RevertingTheBoundPresetKeepsTheBaseline)
{
    const EqPresetCatalog Catalog;
    AutoSwitchController Controller;
    EqState Live = HandTuned();
    Evaluate(Controller, Catalog, FirefoxVocal, Live);

    // Tweak, then re-pick "Vocal" to revert (same path as saving over it).
    Live.Bands[0].GainDb = 9.0f;
    Live.bModified = true;
    PickByHand(Controller, Catalog, Live, "Vocal");
    CHECK(Controller.Source() == EqPresetSource::App); // the binding is still in charge

    Evaluate(Controller, Catalog, Manual, Live);
    CHECK(Live.Bands[3].GainDb == -4.0f); // back to the pre-binding sound, not Vocal
    CHECK(Live.PresetName.empty());
}

TEST(PickingAnotherPresetBecomesTheBaseline)
{
    const EqPresetCatalog Catalog;
    AutoSwitchController Controller;
    EqState Live = HandTuned();
    Evaluate(Controller, Catalog, FirefoxVocal, Live);

    PickByHand(Controller, Catalog, Live, "Flat");
    CHECK(Controller.Source() == EqPresetSource::Manual);
    CHECK(!Evaluate(Controller, Catalog, FirefoxVocal, Live)); // unchanged target: no re-apply
    CHECK(Live.PresetName == "Flat");

    Evaluate(Controller, Catalog, Manual, Live);
    CHECK(Live.PresetName == "Flat"); // the hand pick, not the original hand-tuned curve
}

TEST(PickingByHandWithNoBindingDropsTheSnapshot)
{
    const EqPresetCatalog Catalog;
    AutoSwitchController Controller;
    EqState Live = HandTuned();
    PickByHand(Controller, Catalog, Live, "Vocal");
    CHECK(!Controller.HasSnapshot());
    CHECK(Controller.Source() == EqPresetSource::Manual);
}

TEST(UnbindingTheActiveBindingKeepsTheSound)
{
    const EqPresetCatalog Catalog;
    AutoSwitchController Controller;
    EqState Live = HandTuned();
    Evaluate(Controller, Catalog, HeadphonesBass, Live);
    Evaluate(Controller, Catalog, FirefoxVocal, Live); // app over device
    CHECK(Live.PresetName == "Vocal");

    // Firefox unbound: the device binding underneath is adopted, not applied.
    Controller.OnBindingRemoved(EqPresetSource::App, "Firefox", HeadphonesBass, Live);
    CHECK(Controller.LastTarget() == HeadphonesBass);
    CHECK(Controller.Source() == EqPresetSource::Manual);
    CHECK(!Evaluate(Controller, Catalog, HeadphonesBass, Live));
    CHECK(Live.PresetName == "Vocal");

    // Leaving the device restores the sound at unbind time (the new baseline).
    Evaluate(Controller, Catalog, Manual, Live);
    CHECK(Live.PresetName == "Vocal");
}

TEST(UnbindingAnInactiveBindingChangesNothing)
{
    const EqPresetCatalog Catalog;
    AutoSwitchController Controller;
    EqState Live = HandTuned();
    Evaluate(Controller, Catalog, FirefoxVocal, Live);

    Controller.OnBindingRemoved(EqPresetSource::Device, "headphones", FirefoxVocal, Live);
    CHECK(Controller.LastTarget() == FirefoxVocal);
    CHECK(Controller.Source() == EqPresetSource::App);
    CHECK(Controller.HasSnapshot());
}

TEST(RestoreDropsADeletedPresetName)
{
    EqPresetCatalog Catalog;
    REQUIRE(Catalog.Save("Mine", DefaultHwEqBands(), {}) == EqPresetCatalog::Result::Ok);
    AutoSwitchController Controller;
    EqState Live = HandTuned();
    Live.PresetName = "Mine";
    Live.bModified = true;
    Evaluate(Controller, Catalog, FirefoxVocal, Live);

    REQUIRE(Catalog.Remove("Mine") == EqPresetCatalog::Result::Ok);
    Evaluate(Controller, Catalog, Manual, Live);
    CHECK(Live.Bands[3].GainDb == -4.0f); // the sound comes back...
    CHECK(Live.PresetName.empty());       // ...without the deleted name
    CHECK(!Live.bModified);
}

TEST(SwitchingToTheAlreadyActivePresetKeepsTweaks)
{
    const EqPresetCatalog Catalog;
    AutoSwitchController Controller;
    EqState Live = HandTuned();
    Live.PresetName = "Vocal";
    Live.bModified = true;
    Live.Bands[0].GainDb = 7.0f;
    CHECK(!Evaluate(Controller, Catalog, FirefoxVocal, Live));
    CHECK(Live.Bands[0].GainDb == 7.0f);
    CHECK(Live.bModified);
}

RAMKOLFX_TEST_MAIN()
