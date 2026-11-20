// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// EqPresetCatalog rules (built-in protection, naming, bindings) and
// PresetStore/KeyValueFile persistence.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <vector>

#include <unistd.h>

#include "dsp/graphic_eq_filter.hpp"
#include "ramkolfx/key_value_file.hpp"
#include "preset_store.hpp"
#include "ramkolfx/atomic_file.hpp"
#include "test_harness.hpp"

using namespace ramkolfx;
using Result = EqPresetCatalog::Result;

namespace
{
std::array<EqBand, MaxEqBands> BandsWithGain(size_t Index, float GainDb)
{
    auto Bands = DefaultHwEqBands();
    Bands[Index].GainDb = GainDb;
    return Bands;
}
} // namespace

TEST(NormalizePresetName)
{
    CHECK(NormalizePresetName("  hi \t") == "hi");
    CHECK(!NormalizePresetName(""));
    CHECK(!NormalizePresetName("   "));
    CHECK(!NormalizePresetName("a\nb"));
    CHECK(!NormalizePresetName(std::string(MaxEqPresetNameLength + 1, 'a')));
    CHECK(NormalizePresetName(std::string(MaxEqPresetNameLength, 'a')));
    CHECK(NormalizePresetName("a=b") == "a=b"); // '=' is fine: it's only ever a value, never a key
}

TEST(BuiltInsComeFirstAndAreReadOnly)
{
    EqPresetCatalog Catalog;
    const auto Infos = Catalog.Infos();
    REQUIRE(Infos.size() == BuiltInEqPresets().size());
    CHECK(Infos[0].Name == "Flat" && Infos[0].IsBuiltIn());
    CHECK(Catalog.Save("Flat", DefaultHwEqBands(), {}) == Result::BuiltInReadOnly);
    CHECK(Catalog.Save("  Flat ", DefaultHwEqBands(), {}) == Result::BuiltInReadOnly);
    CHECK(Catalog.Remove("Flat") == Result::BuiltInReadOnly);
    CHECK(Catalog.Remove("nope") == Result::NotFound);
    CHECK(Catalog.Save("", DefaultHwEqBands(), {}) == Result::InvalidName);
}

TEST(SaveSortsOverwritesAndNormalizes)
{
    EqPresetCatalog Catalog;
    std::string Stored;
    CHECK(Catalog.Save("  Zed ", BandsWithGain(3, 4.5f), {}, &Stored) == Result::Ok);
    CHECK(Stored == "Zed");
    CHECK(Catalog.Save("Alpha", DefaultHwEqBands(), {}) == Result::Ok);

    const size_t BuiltInCount = BuiltInEqPresets().size();
    auto Infos = Catalog.Infos();
    REQUIRE(Infos.size() == BuiltInCount + 2);
    CHECK(Infos[BuiltInCount].Name == "Alpha" && Infos[BuiltInCount + 1].Name == "Zed");
    CHECK(Infos[BuiltInCount + 1].Category == EqPresetCategory::User);

    CHECK(Catalog.Save("Zed", BandsWithGain(3, -2.0f), {}) == Result::Ok);
    CHECK(Catalog.Infos().size() == BuiltInCount + 2);
    REQUIRE(Catalog.Find("Zed"));
    CHECK(Catalog.Find("Zed")->Bands[3].GainDb == -2.0f);
}

TEST(BindingsRequireExistingPresetAndFollowDeletes)
{
    EqPresetCatalog Catalog;
    CHECK(Catalog.BindDevice("dev", "Nope") == Result::NotFound);
    CHECK(Catalog.BindDevice("", "Flat") == Result::InvalidName);
    CHECK(Catalog.BindDevice("dev", "Vocal") == Result::Ok);
    CHECK(Catalog.DeviceBinding("dev") == "Vocal");

    CHECK(Catalog.Save("Mine", DefaultHwEqBands(), {}) == Result::Ok);
    CHECK(Catalog.BindApp("Firefox", "Mine") == Result::Ok);
    CHECK(Catalog.BindDevice("dev2", "Mine") == Result::Ok);

    CHECK(Catalog.Remove("Mine") == Result::Ok);
    CHECK(Catalog.AppBindings().empty());
    CHECK(Catalog.DeviceBinding("dev2").empty());
    CHECK(Catalog.DeviceBinding("dev") == "Vocal");

    CHECK(Catalog.BindDevice("dev", "") == Result::Ok); // unbind
    CHECK(Catalog.DeviceBindings().empty());
}

TEST(TargetNamesAreValidated)
{
    CHECK(IsValidTargetName("alsa_output.usb-Generic_USB_Audio-00.HiFi__Speaker__sink"));
    CHECK(IsValidTargetName("a=b"));
    CHECK(!IsValidTargetName(""));
    CHECK(!IsValidTargetName("foo\npreset.0.name=X"));
    CHECK(!IsValidTargetName(std::string("a\0b", 3)));
    CHECK(IsValidTargetName(std::string(MaxTargetNameLength, 'a')));
    CHECK(!IsValidTargetName(std::string(MaxTargetNameLength + 1, 'a')));

    // A name that could inject config keys never reaches a binding.
    EqPresetCatalog Catalog;
    CHECK(Catalog.BindApp("foo\nbar", "Flat") == Result::InvalidName);
    CHECK(Catalog.BindDevice("dev\r", "Flat") == Result::InvalidName);
    CHECK(Catalog.AppBindings().empty() && Catalog.DeviceBindings().empty());
}

TEST(DanglingBindingsDroppedOnConstruction)
{
    PresetFileContents Contents;
    Contents.DeviceBindings["x"] = "Ghost";
    Contents.AppBindings["y"] = "Flat";
    EqPresetCatalog Catalog(Contents);
    CHECK(Catalog.DeviceBindings().empty());
    CHECK(Catalog.AppBinding("y") == "Flat");
}

TEST(PresetStoreRoundTrip)
{
    test::TempPath Path;
    EqPresetCatalog Catalog;
    BassEnhancerSettings Bass{true, 70.0f, 0.8f, 0.25f};
    auto Bands = BandsWithGain(3, -2.0f);
    Bands[7].FilterType = EqFilterType::Notch;
    CHECK(Catalog.Save("Mine", Bands, Bass) == Result::Ok);
    CHECK(Catalog.Save("Other = weird", DefaultHwEqBands(), {}) == Result::Ok);
    CHECK(Catalog.BindDevice("alsa_out=1", "Mine") == Result::Ok);
    CHECK(Catalog.BindApp("mpv", "Vocal") == Result::Ok);

    PresetStore Store(Path.Get());
    CHECK(Store.Save(Catalog.ToFileContents()));

    EqPresetCatalog Loaded(Store.Load());
    const EqPreset* Mine = Loaded.Find("Mine");
    REQUIRE(Mine);
    CHECK(!Mine->IsBuiltIn());
    CHECK(Mine->Bands[3].GainDb == -2.0f && Mine->Bands[7].FilterType == EqFilterType::Notch);
    CHECK(Mine->Bass.bEnabled && Mine->Bass.CutoffHz == 70.0f && Mine->Bass.Mix == 0.25f);
    CHECK(Loaded.Find("Other = weird"));
    CHECK(Loaded.DeviceBinding("alsa_out=1") == "Mine");
    CHECK(Loaded.AppBinding("mpv") == "Vocal");
}

TEST(AutoDetectAndPresetBindingsExcludeEachOther)
{
    EqPresetCatalog Catalog;
    CHECK(Catalog.BindApp("Firefox", "Vocal") == Result::Ok);
    CHECK(Catalog.BindAppAutoDetect("Firefox") == Result::Ok);
    CHECK(Catalog.IsAppAutoDetect("Firefox") && Catalog.AppBinding("Firefox").empty());
    CHECK(Catalog.BindApp("Firefox", "Vocal") == Result::Ok);
    CHECK(!Catalog.IsAppAutoDetect("Firefox") && Catalog.AppBinding("Firefox") == "Vocal");

    CHECK(Catalog.BindDeviceAutoDetect("speakers") == Result::Ok);
    CHECK(Catalog.BindDevice("speakers", "") == Result::Ok); // unbind clears Auto too
    CHECK(Catalog.AutoDetectDevices().empty());
    CHECK(Catalog.BindDeviceAutoDetect("bad\nname") == Result::InvalidName);

    // A hand-edited file with both: the preset binding wins.
    PresetFileContents Contents;
    Contents.AppBindings = {{"mpv", "Flat"}};
    Contents.AutoDetectApps = {"mpv", "vlc"};
    const EqPresetCatalog Both(std::move(Contents));
    CHECK(Both.AppBinding("mpv") == "Flat" && !Both.IsAppAutoDetect("mpv"));
    CHECK(Both.IsAppAutoDetect("vlc"));
}

TEST(DetectedClassTableDefaultsAndOverrides)
{
    EqPresetCatalog Catalog;
    CHECK(Catalog.DetectedClassPreset(DetectedClass::Speech) == "Spoken Word");
    CHECK(Catalog.DetectedClassPreset(DetectedClass::Movie) == "Movie");
    CHECK(Catalog.DetectedClassPreset(DetectedClass::Music).empty());
    CHECK(Catalog.DetectedClassPreset(DetectedClass::None).empty());
    // Every genre has a same-named built-in.
    for (size_t i = static_cast<size_t>(DetectedClass::Rock); i < DetectedClassCount; ++i)
    {
        const std::string Preset = Catalog.DetectedClassPreset(static_cast<DetectedClass>(i));
        CHECK(FindBuiltInEqPreset(Preset) != nullptr);
    }

    CHECK(Catalog.BindDetectedClass(DetectedClass::None, "Flat") == Result::InvalidName);
    CHECK(Catalog.BindDetectedClass(DetectedClass::Jazz, "Ghost") == Result::NotFound);
    CHECK(Catalog.BindDetectedClass(DetectedClass::Speech, "") == Result::Ok); // no switch
    CHECK(Catalog.DetectedClassPreset(DetectedClass::Speech).empty());
    CHECK(Catalog.BindDetectedClass(DetectedClass::Speech, "Spoken Word") == Result::Ok); // back to default
    CHECK(Catalog.ToFileContents().DetectedClassOverrides.empty());

    // Deleting a preset a class points at turns that class into no switch.
    REQUIRE(Catalog.Save("Mine", DefaultHwEqBands(), {}) == Result::Ok);
    CHECK(Catalog.BindDetectedClass(DetectedClass::Rock, "Mine") == Result::Ok);
    CHECK(Catalog.Remove("Mine") == Result::Ok);
    CHECK(Catalog.DetectedClassPreset(DetectedClass::Rock).empty());
}

TEST(AutoDetectAndClassTableRoundTrip)
{
    test::TempPath Path;
    EqPresetCatalog Catalog;
    REQUIRE(Catalog.Save("Mine", DefaultHwEqBands(), {}) == Result::Ok);
    CHECK(Catalog.BindAppAutoDetect("Firefox = browser") == Result::Ok);
    CHECK(Catalog.BindDeviceAutoDetect("alsa_out") == Result::Ok);
    CHECK(Catalog.BindDetectedClass(DetectedClass::HipHop, "Mine") == Result::Ok);
    CHECK(Catalog.BindDetectedClass(DetectedClass::Speech, "") == Result::Ok);

    PresetStore Store(Path.Get());
    CHECK(Store.Save(Catalog.ToFileContents()));
    const EqPresetCatalog Loaded(Store.Load());
    CHECK(Loaded.IsAppAutoDetect("Firefox = browser"));
    CHECK(Loaded.IsDeviceAutoDetect("alsa_out"));
    CHECK(Loaded.DetectedClassPreset(DetectedClass::HipHop) == "Mine");
    CHECK(Loaded.DetectedClassPreset(DetectedClass::Speech).empty());
    CHECK(Loaded.DetectedClassPreset(DetectedClass::Jazz) == "Jazz");
}

TEST(ClassTableFollowsRenamesAndDropsDanglingEntries)
{
    test::TempPath Path;
    {
        std::ofstream File(Path.Get());
        File << "preset.count=1\n"
             << "preset.0.name=Rock\n" // a user preset a built-in now shadows
             << "detected.metal=Rock\n"
             << "detected.jazz=Ghost\n"
             << "detected.someday=Flat\n"; // a class from a newer build
    }
    const EqPresetCatalog Catalog(PresetStore(Path.Get()).Load());
    CHECK(Catalog.DetectedClassPreset(DetectedClass::Metal) == "Rock (custom)");
    CHECK(Catalog.DetectedClassPreset(DetectedClass::Jazz).empty());
}

TEST(BuiltInPresetsAreWellFormed)
{
    const auto& BuiltIns = BuiltInEqPresets();
    REQUIRE(BuiltIns.size() > 3);
    CHECK(BuiltIns[0].Name == "Flat");
    CHECK(&BuiltInEqPresets() == &BuiltIns); // built once, shared
    CHECK(FindBuiltInEqPreset("Rock") && !FindBuiltInEqPreset("rock"));
    for (size_t i = 0; i < BuiltIns.size(); ++i)
    {
        const EqPreset& Preset = BuiltIns[i];
        CHECK(NormalizePresetName(Preset.Name) == Preset.Name);
        CHECK(Preset.IsBuiltIn());
        // Grouped: categories never go backwards, so each gets one header.
        CHECK(i == 0 || Preset.Category >= BuiltIns[i - 1].Category);
        for (size_t j = i + 1; j < BuiltIns.size(); ++j)
        {
            CHECK(BuiltIns[j].Name != Preset.Name);
        }
        // Within what the GUI's sliders can show and reproduce.
        for (const EqBand& Band : Preset.Bands)
        {
            CHECK(Band.GainDb >= -12.0f && Band.GainDb <= 12.0f);
        }
        CHECK(Preset.Bass.CutoffHz >= 40.0f && Preset.Bass.CutoffHz <= 250.0f);
        CHECK(Preset.Bass.Drive >= 0.0f && Preset.Bass.Drive <= 1.0f);
        CHECK(Preset.Bass.Mix >= 0.0f && Preset.Bass.Mix <= 1.0f);
    }
    for (EqPresetCategory Category : {EqPresetCategory::General, EqPresetCategory::Music, EqPresetCategory::Games})
    {
        CHECK(std::any_of(BuiltIns.begin(), BuiltIns.end(),
                          [&](const EqPreset& P) { return P.Category == Category; }));
    }
}

TEST(BuiltInCurvesPeakAtMostSixDb)
{
    // There's no preamp or limiter after the EQ (docs/eq-presets-plan.md),
    // so hold every built-in's actual summed response - neighbouring bands
    // overlap - to +6 dB. Measured on the real filter: its impulse
    // response's spectrum on a 1/24-octave grid, 20 Hz..20 kHz.
    constexpr float SampleRate = 48000.0f;
    constexpr uint32 Frames = 24000; // the lowest band's ringing is long gone by 0.5 s
    for (const EqPreset& Preset : BuiltInEqPresets())
    {
        std::vector<float> Impulse(Frames * 2, 0.0f);
        Impulse[0] = Impulse[1] = 1.0f;
        GraphicEqFilter(Preset.Bands, SampleRate).Process(Impulse.data(), 2, Impulse.data(), 2, Frames);

        double PeakDb = -100.0;
        for (double Hz = 20.0; Hz < 20000.0; Hz *= std::pow(2.0, 1.0 / 24.0))
        {
            const std::complex<double> Step = std::polar(1.0, -2.0 * M_PI * Hz / SampleRate);
            std::complex<double> Phase = 1.0;
            std::complex<double> Sum = 0.0;
            for (uint32 i = 0; i < Frames; ++i)
            {
                Sum += static_cast<double>(Impulse[i * 2]) * Phase;
                Phase *= Step;
            }
            PeakDb = std::max(PeakDb, 20.0 * std::log10(std::abs(Sum)));
        }
        if (PeakDb > 6.0)
        {
            fprintf(stderr, "  %s peaks at %+.2f dB\n", Preset.Name.c_str(), PeakDb);
        }
        CHECK(PeakDb <= 6.0);
    }
}

TEST(UserPresetShadowedByNewBuiltInIsRenamed)
{
    // A presets.conf from before "Rock" and "Jazz" were built-ins, where
    // "Rock (custom)" is also taken further down the file.
    test::TempPath Path;
    {
        std::ofstream File(Path.Get());
        File << "preset.count=3\n"
             << "preset.0.name=Rock\n"
             << "preset.0.hweq.0.gain=7\n"
             << "preset.1.name=Jazz\n"
             << "preset.2.name=Rock (custom)\n"
             << "device_binding.count=1\n"
             << "device_binding.0.target=headphones\n"
             << "device_binding.0.preset=Rock\n"
             << "app_binding.count=2\n"
             << "app_binding.0.target=mpv\n"
             << "app_binding.0.preset=Jazz\n"
             << "app_binding.1.target=Firefox\n"
             << "app_binding.1.preset=Rock (custom)\n";
    }
    PresetStore Store(Path.Get());
    EqPresetCatalog Catalog(Store.Load());
    CHECK(Catalog.RenamedUserPresets());
    const EqPreset* Mine = Catalog.Find("Rock (custom 2)");
    REQUIRE(Mine);
    CHECK(!Mine->IsBuiltIn() && Mine->Bands[0].GainDb == 7.0f);
    REQUIRE(Catalog.Find("Rock"));
    CHECK(Catalog.Find("Rock")->IsBuiltIn());
    CHECK(Catalog.Find("Rock (custom)") && Catalog.Find("Jazz (custom)"));
    CHECK(Catalog.DeviceBinding("headphones") == "Rock (custom 2)");
    CHECK(Catalog.AppBinding("mpv") == "Jazz (custom)");
    CHECK(Catalog.AppBinding("Firefox") == "Rock (custom)");

    // settings.conf's active preset follows the rename when it named the
    // user's preset - flagged so, or from a file older than the flag.
    CHECK(Catalog.ResolveStoredName("Rock", false) == "Rock (custom 2)");
    CHECK(Catalog.ResolveStoredName("Rock", std::nullopt) == "Rock (custom 2)");
    CHECK(Catalog.ResolveStoredName("Rock", true) == "Rock");
    CHECK(Catalog.ResolveStoredName("Rock (custom)", false) == "Rock (custom)");

    // Saved back, it loads with nothing left to rename.
    CHECK(Store.Save(Catalog.ToFileContents()));
    const EqPresetCatalog Reloaded(Store.Load());
    CHECK(!Reloaded.RenamedUserPresets());
    CHECK(Reloaded.ToFileContents().UserPresets.size() == 3);
    CHECK(Reloaded.DeviceBinding("headphones") == "Rock (custom 2)");
}

TEST(StoredActivePresetNeverPicksUpANewBuiltIn)
{
    // presets.conf missing or unreadable: nothing says the user had a
    // "Rock", but settings.conf does - its curve is theirs, not the
    // built-in's, so the name must not resolve to the built-in.
    EqPresetCatalog Catalog;
    REQUIRE(Catalog.Save("Mine", DefaultHwEqBands(), {}) == Result::Ok);
    CHECK(Catalog.ResolveStoredName("Rock", false).empty());
    CHECK(Catalog.ResolveStoredName("Rock", std::nullopt).empty()); // pre-flag file: Rock wasn't a built-in then
    CHECK(Catalog.ResolveStoredName("Rock", true) == "Rock");
    CHECK(Catalog.ResolveStoredName("Vocal", std::nullopt) == "Vocal"); // was a built-in then too
    CHECK(Catalog.ResolveStoredName("Mine", false) == "Mine");
    CHECK(Catalog.ResolveStoredName("Mine", std::nullopt) == "Mine");
    CHECK(Catalog.ResolveStoredName("Mine", true).empty());
    CHECK(Catalog.ResolveStoredName("Gone", false).empty());
    CHECK(Catalog.ResolveStoredName("", std::nullopt).empty());
}

TEST(PresetStoreMissingAndCorruptFiles)
{
    test::TempPath Path;
    CHECK(PresetStore(Path.Get()).Load().UserPresets.empty());

    // Garbage lines are ignored, bad entries skipped, good ones kept.
    {
        std::ofstream File(Path.Get());
        File << "not a key value line\n"
             << "preset.count=4\n"
             << "preset.0.name=   \n"       // invalid
             << "preset.1.name=Good\n"
             << "preset.1.hweq.2.gain=oops\n" // unparseable -> default
             << "preset.1.hweq.3.gain=5\n"
             << "preset.1.hweq.4.type=99\n"   // out of range -> default
             << "preset.2.name=Good\n"      // duplicate
             << "preset.3.name=\n";         // invalid
    }
    const auto Contents = PresetStore(Path.Get()).Load();
    REQUIRE(Contents.UserPresets.size() == 1);
    const EqPreset& Good = Contents.UserPresets[0];
    CHECK(Good.Name == "Good");
    CHECK(Good.Bands[2].GainDb == 0.0f);
    CHECK(Good.Bands[3].GainDb == 5.0f);
    CHECK(Good.Bands[4].FilterType == EqFilterType::Peaking);
}

TEST(AtomicWriteCreatesMissingDirectories)
{
    // A config dir that was never created, or was deleted while running -
    // two levels deep, like ~/.config/ramkolfx on a fresh account.
    test::TempPath Root;
    const std::string Parent = Root.Get() + "/config";
    const std::string Dir = Parent + "/ramkolfx";
    const std::string File = Dir + "/settings.conf";
    CHECK(WriteFileAtomically(File, "a=1\n"));
    const auto Loaded = KeyValueFile::Load(File);
    REQUIRE(Loaded);
    CHECK(Loaded->GetLong("a", 0) == 1);

    std::remove(File.c_str());
    rmdir(Dir.c_str());
    rmdir(Parent.c_str());
    rmdir(Root.Get().c_str());
}

TEST(KeyValueFileBasics)
{
    test::TempPath Path;
    CHECK(!KeyValueFile::Load(Path.Get()));
    CHECK(WriteFileAtomically(Path.Get(), "a=1\nb=x=y\nc=2.5\nflag=1\n"));
    const auto File = KeyValueFile::Load(Path.Get());
    REQUIRE(File);
    CHECK(File->GetLong("a", 0) == 1);
    CHECK(File->GetString("b", "") == "x=y");
    CHECK(File->GetFloat("c", 0.0f) == 2.5f);
    CHECK(File->GetBool("flag", false));
    CHECK(File->GetLong("missing", 7) == 7);
    CHECK(File->GetLong("b", 9) == 9); // unparseable -> default
    CHECK(File->Has("a") && !File->Has("missing"));
}

RAMKOLFX_TEST_MAIN()
