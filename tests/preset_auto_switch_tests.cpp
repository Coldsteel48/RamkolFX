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
const EqPresetCatalog BoundCatalog = []
{
    PresetFileContents Contents;
    Contents.DeviceBindings = {{"headphones", "Flat"}};
    Contents.AppBindings = {{"Firefox", "Vocal"}, {"mpv", "Bass Boost"}};
    return EqPresetCatalog(std::move(Contents));
}();

AutoSwitchTarget Resolve(const AppActivityTracker& Tracker, const std::string& Device)
{
    return ResolveBinding(Tracker, BoundCatalog, Device, true).AsTarget();
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

TEST(PendingTargetKeepsCurveAndWhoIsInCharge)
{
    const EqPresetCatalog Catalog;
    AutoSwitchController Controller;
    EqState Live = HandTuned();
    const AutoSwitchTarget Pending{EqPresetSource::App, "Firefox", ""};
    const AutoSwitchTarget Jazz{EqPresetSource::App, "Firefox", "Jazz"};

    // Auto binding comes into effect, nothing detected yet: snapshot only.
    CHECK(!Evaluate(Controller, Catalog, Pending, Live));
    CHECK(Controller.HasSnapshot() && Controller.Source() == EqPresetSource::App);
    CHECK(Live.Bands[3].GainDb == -4.0f);

    CHECK(Evaluate(Controller, Catalog, Jazz, Live));
    CHECK(Live.PresetName == "Jazz");

    // The user picks Rock by hand, then detection is held (pending again):
    // nothing moves and the user stays in charge.
    PickByHand(Controller, Catalog, Live, "Rock");
    CHECK(!Evaluate(Controller, Catalog, Pending, Live));
    CHECK(Live.PresetName == "Rock");
    CHECK(Controller.Source() == EqPresetSource::Manual);

    // Binding stops applying: the pre-binding sound comes back... to the
    // user's choice, which became the baseline.
    CHECK(Evaluate(Controller, Catalog, Manual, Live));
    CHECK(Live.PresetName == "Rock");
}

// --- Auto-detect bindings ---------------------------------------------------

namespace
{
const EqPresetCatalog AutoCatalog = []
{
    PresetFileContents Contents;
    Contents.DeviceBindings = {{"headphones", "Flat"}};
    Contents.AppBindings = {{"mpv", "Bass Boost"}};
    Contents.AutoDetectDevices = {"speakers"};
    Contents.AutoDetectApps = {"Firefox"};
    return EqPresetCatalog(std::move(Contents));
}();

ClassScores Scores(std::initializer_list<std::pair<DetectedClass, float>> Values)
{
    ClassScores Out{};
    for (const auto& [Class, Score] : Values)
    {
        ScoreOf(Out, Class) = Score;
    }
    return Out;
}

const ClassScores JazzScores =
    Scores({{DetectedClass::Music, 0.9f}, {DetectedClass::Jazz, 0.8f}});
const ClassScores SpeechScores = Scores({{DetectedClass::Speech, 0.9f}});

// Feeds Scores every 2s from FromMs until (inclusive) ToMs.
void Feed(AutoDetectSession& Session, const ClassScores& S, int64_t FromMs, int64_t ToMs)
{
    for (int64_t T = FromMs; T <= ToMs; T += 2000)
    {
        Session.AddScores(S, T);
    }
}
} // namespace

TEST(AutoBindingsResolveLikeAnyOther)
{
    AppActivityTracker Tracker;
    Tracker.Update({"Firefox"}, 100);
    CHECK((ResolveBinding(Tracker, AutoCatalog, "headphones", true) ==
           ResolvedBinding{EqPresetSource::App, "Firefox", "", true, 100}));

    // mpv started later: its preset binding wins.
    Tracker.Update({"Firefox", "mpv"}, 200);
    CHECK(ResolveBinding(Tracker, AutoCatalog, "", true).Key == "mpv");

    // Device Auto, nothing playing.
    AppActivityTracker Idle;
    CHECK((ResolveBinding(Idle, AutoCatalog, "speakers", true) ==
           ResolvedBinding{EqPresetSource::Device, "speakers", "", true, 0}));
}

TEST(AutoBindingsCountAsUnboundWhenUnavailable)
{
    AppActivityTracker Tracker;
    Tracker.Update({"Firefox"}, 0);
    CHECK((ResolveBinding(Tracker, AutoCatalog, "headphones", false).AsTarget() ==
           AutoSwitchTarget{EqPresetSource::Device, "headphones", "Flat"}));
    CHECK(ResolveBinding(Tracker, AutoCatalog, "speakers", false) == ResolvedBinding{});
}

TEST(SessionGoesFromPendingToDetectedPreset)
{
    AutoDetectSession Session;
    const ResolvedBinding Firefox{EqPresetSource::App, "Firefox", "", true, 0};
    CHECK((Session.Update(Firefox, AutoCatalog) == AutoSwitchTarget{EqPresetSource::App, "Firefox", ""}));
    CHECK(Session.ShouldDetect());

    Feed(Session, JazzScores, 0, 4000);
    CHECK(Session.Current() == DetectedClass::Jazz);
    CHECK((Session.Update(Firefox, AutoCatalog) == AutoSwitchTarget{EqPresetSource::App, "Firefox", "Jazz"}));
}

TEST(SessionPassesPresetBindingsThroughAndStopsDetecting)
{
    AutoDetectSession Session;
    Session.Update({EqPresetSource::App, "Firefox", "", true, 0}, AutoCatalog);
    const ResolvedBinding Mpv{EqPresetSource::App, "mpv", "Bass Boost", false, 0};
    CHECK((Session.Update(Mpv, AutoCatalog) == AutoSwitchTarget{EqPresetSource::App, "mpv", "Bass Boost"}));
    CHECK(!Session.ShouldDetect() && !Session.IsActive());
    CHECK(Session.Update(ResolvedBinding{}, AutoCatalog) == AutoSwitchTarget{});
}

TEST(NewSpellOrBindingStartsDetectionAfresh)
{
    AutoDetectSession Session;
    const ResolvedBinding Spell1{EqPresetSource::App, "Firefox", "", true, 0};
    Session.Update(Spell1, AutoCatalog);
    Feed(Session, JazzScores, 0, 4000);
    CHECK(Session.Current() == DetectedClass::Jazz);

    // Same binding, same spell: kept.
    Session.Update(Spell1, AutoCatalog);
    CHECK(Session.Current() == DetectedClass::Jazz);

    // Firefox restarted after the grace period: fresh.
    const ResolvedBinding Spell2{EqPresetSource::App, "Firefox", "", true, 60000};
    CHECK(Session.Update(Spell2, AutoCatalog).PresetName.empty());
    CHECK(Session.Current() == DetectedClass::None);

    // A device Auto binding taking over: fresh too.
    Feed(Session, JazzScores, 60000, 64000);
    Session.Update({EqPresetSource::Device, "speakers", "", true, 0}, AutoCatalog);
    CHECK(Session.Current() == DetectedClass::None);
}

TEST(HoldFreezesTheSpellUntilTheBindingChanges)
{
    AutoDetectSession Session;
    const ResolvedBinding Firefox{EqPresetSource::App, "Firefox", "", true, 0};
    Session.Update(Firefox, AutoCatalog);
    Feed(Session, JazzScores, 0, 4000);
    Session.Hold();
    CHECK(Session.IsHeld() && !Session.ShouldDetect());
    CHECK(Session.Update(Firefox, AutoCatalog).PresetName.empty());

    // Results arriving while held are ignored.
    Feed(Session, SpeechScores, 6000, 60000);
    CHECK(Session.Current() == DetectedClass::Jazz);

    // Next spell: detection resumes.
    Session.Update({EqPresetSource::App, "Firefox", "", true, 90000}, AutoCatalog);
    CHECK(!Session.IsHeld() && Session.ShouldDetect());
}

TEST(HoldWithoutAnAutoBindingDoesNothing)
{
    AutoDetectSession Session;
    Session.Update({EqPresetSource::App, "mpv", "Bass Boost", false, 0}, AutoCatalog);
    Session.Hold();
    CHECK(!Session.IsHeld());
    Session.Update({EqPresetSource::App, "Firefox", "", true, 0}, AutoCatalog);
    CHECK(Session.ShouldDetect());
}

TEST(ClassTableDecidesThePreset)
{
    EqPresetCatalog Catalog;
    REQUIRE(Catalog.BindAppAutoDetect("Firefox") == EqPresetCatalog::Result::Ok);
    REQUIRE(Catalog.Save("My Jazz", DefaultHwEqBands(), {}) == EqPresetCatalog::Result::Ok);
    REQUIRE(Catalog.BindDetectedClass(DetectedClass::Jazz, "My Jazz") == EqPresetCatalog::Result::Ok);
    AutoDetectSession Session;
    const ResolvedBinding Firefox{EqPresetSource::App, "Firefox", "", true, 0};
    Session.Update(Firefox, Catalog);
    Feed(Session, JazzScores, 0, 4000);
    CHECK(Session.Update(Firefox, Catalog).PresetName == "My Jazz");

    // Plain Music (genre unsure) switches nothing by default.
    AutoDetectSession Unsure;
    Unsure.Update(Firefox, Catalog);
    Feed(Unsure, Scores({{DetectedClass::Music, 0.9f}}), 0, 4000);
    CHECK(Unsure.Current() == DetectedClass::Music);
    CHECK(Unsure.Update(Firefox, Catalog).PresetName.empty());
}

TEST(EndToEndAutoBindingSwitchesAndRestores)
{
    AutoSwitchController Controller;
    AutoDetectSession Session;
    EqState Live = HandTuned();
    AppActivityTracker Tracker;

    const auto Tick = [&](int64_t NowMs, std::vector<std::string> Playing)
    {
        Tracker.Update(Playing, NowMs);
        const AutoSwitchTarget Target =
            Session.Update(ResolveBinding(Tracker, AutoCatalog, "", true), AutoCatalog);
        return Evaluate(Controller, AutoCatalog, Target, Live);
    };

    CHECK(!Tick(0, {"Firefox"})); // pending: curve untouched
    CHECK(Live.Bands[3].GainDb == -4.0f);
    Feed(Session, SpeechScores, 0, 4000);
    CHECK(Tick(4000, {"Firefox"}));
    CHECK(Live.PresetName == "Spoken Word");

    CHECK(Tick(10000, {})); // grace over: back to the hand-tuned curve
    CHECK(Live.Bands[3].GainDb == -4.0f && Live.PresetName.empty());
}

RAMKOLFX_TEST_MAIN()
