// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// SettingsStore persistence, in particular the per-device spatial modes.

#include <fstream>

#include "settings_store.hpp"
#include "test_harness.hpp"

using namespace ramkolfx;
using test::TempPath;

TEST(MissingFileLoadsNothing)
{
    TempPath Path;
    CHECK(!SettingsStore(Path.Get()).Load());
}

TEST(RoundTripKeepsDeviceModes)
{
    TempPath Path;
    PersistedSettings In;
    In.Mode = SpatialMode::Basic;
    In.OutputDeviceName = "alsa_output.speakers";
    In.ActivePresetName = "Vocal";
    In.bActivePresetBuiltIn = true;
    In.DeviceSpatialModes = {{"alsa_output.speakers", SpatialMode::Off},
                             {"bluez_output.headphones", SpatialMode::Advanced},
                             {"weird=name", SpatialMode::Basic}};
    In.AutoDetectDevice = InferenceDevice::Npu;

    const SettingsStore Store(Path.Get());
    Store.Save(In);
    const auto Out = Store.Load();
    REQUIRE(Out);
    CHECK(Out->Mode == SpatialMode::Basic);
    CHECK(Out->OutputDeviceName == "alsa_output.speakers");
    CHECK(Out->ActivePresetName == "Vocal");
    CHECK(Out->bActivePresetBuiltIn == std::optional<bool>(true));
    CHECK(Out->DeviceSpatialModes == In.DeviceSpatialModes);
    CHECK(Out->AutoDetectDevice == InferenceDevice::Npu);
}

TEST(OldFileWithoutDeviceModesLoads)
{
    TempPath Path;
    {
        std::ofstream File(Path.Get());
        File << "mode=2\noutput_device=x\n";
    }
    const auto Out = SettingsStore(Path.Get()).Load();
    REQUIRE(Out);
    CHECK(Out->Mode == SpatialMode::Advanced);
    CHECK(Out->DeviceSpatialModes.empty());
    CHECK(!Out->bActivePresetBuiltIn); // unknown, not "user preset"
}

TEST(AutoDetectTimingRoundTripsAndIsClamped)
{
    TempPath Path;
    PersistedSettings In;
    In.DetectTiming = {800, 1500, 0, 2.5f};
    const SettingsStore Store(Path.Get());
    Store.Save(In);
    auto Out = Store.Load();
    REQUIRE(Out);
    CHECK(Out->DetectTiming == In.DetectTiming);

    {
        std::ofstream File(Path.Get());
        File << "mode=0\nauto_detect.check_every_ms=10\nauto_detect.switch_after_ms=-5\n"
                "auto_detect.keep_at_least_ms=999999\nauto_detect.cpu_limit_percent=50\n";
    }
    Out = Store.Load();
    REQUIRE(Out);
    CHECK(Out->DetectTiming.CheckEveryMs == MinCheckEveryMs);
    CHECK(Out->DetectTiming.SwitchAfterMs == AutoDetectTiming{}.SwitchAfterMs); // unreadable: the default
    CHECK(Out->DetectTiming.KeepAtLeastMs == MaxKeepAtLeastMs);
    CHECK(Out->DetectTiming.CpuLimitPercent == MaxCpuLimitPercent);
}

TEST(UnknownAutoDetectDeviceFallsBackToAuto)
{
    TempPath Path;
    {
        std::ofstream File(Path.Get());
        File << "mode=0\nauto_detect.device=tpu\n";
    }
    const auto Out = SettingsStore(Path.Get()).Load();
    REQUIRE(Out);
    CHECK(Out->AutoDetectDevice == InferenceDevice::Auto);
}

TEST(InvalidDeviceModesAreDropped)
{
    TempPath Path;
    {
        std::ofstream File(Path.Get());
        File << "mode=9\n"
                "device_mode.count=3\n"
                "device_mode.0.device=good\ndevice_mode.0.mode=1\n"
                "device_mode.1.device=bad\ndevice_mode.1.mode=7\n"
                "device_mode.2.device=\ndevice_mode.2.mode=1\n";
    }
    const auto Out = SettingsStore(Path.Get()).Load();
    REQUIRE(Out);
    CHECK(Out->Mode == SpatialMode::Off); // out-of-range mode falls back to the default
    REQUIRE(Out->DeviceSpatialModes.size() == 1);
    CHECK(Out->DeviceSpatialModes.at("good") == SpatialMode::Basic);
}

TEST(HugeCountsAreClamped)
{
    TempPath Path;
    {
        std::ofstream File(Path.Get());
        File << "device_mode.count=999999999999\n"
                "device_mode.0.device=good\ndevice_mode.0.mode=2\n";
    }
    const auto Out = SettingsStore(Path.Get()).Load(); // must return promptly
    REQUIRE(Out);
    REQUIRE(Out->DeviceSpatialModes.size() == 1);
    CHECK(Out->DeviceSpatialModes.at("good") == SpatialMode::Advanced);
}

TEST(NewDeviceRemembersTheLiveMode)
{
    std::map<std::string, SpatialMode> Modes;
    SpatialMode Mode = SpatialMode::Basic;
    CHECK(ApplyRememberedSpatialMode(Modes, "speakers", Mode));
    CHECK(Mode == SpatialMode::Basic);
    CHECK(Modes.at("speakers") == SpatialMode::Basic);
}

TEST(KnownDeviceRestoresItsMode)
{
    std::map<std::string, SpatialMode> Modes{{"headphones", SpatialMode::Advanced}};
    SpatialMode Mode = SpatialMode::Off;
    CHECK(!ApplyRememberedSpatialMode(Modes, "headphones", Mode));
    CHECK(Mode == SpatialMode::Advanced);
}

TEST(InvalidDeviceIsIgnored)
{
    std::map<std::string, SpatialMode> Modes;
    SpatialMode Mode = SpatialMode::Basic;
    CHECK(!ApplyRememberedSpatialMode(Modes, "", Mode));
    CHECK(!ApplyRememberedSpatialMode(Modes, "bad\nname", Mode));
    CHECK(Mode == SpatialMode::Basic);
    CHECK(Modes.empty());
}

RAMKOLFX_TEST_MAIN()
