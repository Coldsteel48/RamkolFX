// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// EqPresetCatalog rules (built-in protection, naming, bindings) and
// PresetStore/KeyValueFile persistence.

#include <fstream>

#include "dsp/graphic_eq_filter.hpp"
#include "key_value_file.hpp"
#include "preset_store.hpp"
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
    REQUIRE(Infos.size() == 3);
    CHECK(Infos[0].Name == "Flat" && Infos[0].bBuiltIn);
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

    auto Infos = Catalog.Infos();
    REQUIRE(Infos.size() == 5);
    CHECK(Infos[3].Name == "Alpha" && Infos[4].Name == "Zed" && !Infos[4].bBuiltIn);

    CHECK(Catalog.Save("Zed", BandsWithGain(3, -2.0f), {}) == Result::Ok);
    CHECK(Catalog.Infos().size() == 5);
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
    CHECK(!Mine->bBuiltIn);
    CHECK(Mine->Bands[3].GainDb == -2.0f && Mine->Bands[7].FilterType == EqFilterType::Notch);
    CHECK(Mine->Bass.bEnabled && Mine->Bass.CutoffHz == 70.0f && Mine->Bass.Mix == 0.25f);
    CHECK(Loaded.Find("Other = weird"));
    CHECK(Loaded.DeviceBinding("alsa_out=1") == "Mine");
    CHECK(Loaded.AppBinding("mpv") == "Vocal");
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
             << "preset.0.name=Flat\n"      // collides with a built-in
             << "preset.1.name=Good\n"
             << "preset.1.hweq.2.gain=oops\n" // unparseable -> default
             << "preset.1.hweq.3.gain=5\n"
             << "preset.1.hweq.4.type=99\n"   // out of range -> default
             << "preset.2.name=Good\n"      // duplicate
             << "preset.3.name=   \n";      // invalid
    }
    const auto Contents = PresetStore(Path.Get()).Load();
    REQUIRE(Contents.UserPresets.size() == 1);
    const EqPreset& Good = Contents.UserPresets[0];
    CHECK(Good.Name == "Good");
    CHECK(Good.Bands[2].GainDb == 0.0f);
    CHECK(Good.Bands[3].GainDb == 5.0f);
    CHECK(Good.Bands[4].FilterType == EqFilterType::Peaking);
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
