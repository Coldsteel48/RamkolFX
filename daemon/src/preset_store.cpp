// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "preset_store.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include <sys/stat.h>

#include "dsp/graphic_eq_filter.hpp"
#include "key_value_file.hpp"
#include "xdg_paths.hpp"

namespace ramkolfx
{

namespace
{

std::string DefaultPresetsPath()
{
    std::string ConfigDir = XdgConfigHome() + "/ramkolfx";
    mkdir(ConfigDir.c_str(), 0700);
    return ConfigDir + "/presets.conf";
}

std::string BandKey(const std::string& Prefix, size_t Index, const char* Field)
{
    return Prefix + "hweq." + std::to_string(Index) + "." + Field;
}

// Builds a built-in preset from per-band gains on the default ISO graphic
// EQ curve (same frequencies/Q the GUI's simple-mode sliders use), so
// every built-in is reproducible by hand in simple mode.
EqPreset MakeGraphicPreset(const char* Name, const std::array<float, MaxEqBands>& GainsDb,
                           const BassEnhancerSettings& Bass)
{
    EqPreset Preset;
    Preset.Name = Name;
    Preset.Bands = DefaultHwEqBands();
    for (size_t i = 0; i < MaxEqBands; ++i)
    {
        Preset.Bands[i].GainDb = GainsDb[i];
    }
    Preset.Bass = Bass;
    Preset.bBuiltIn = true;
    return Preset;
}

bool ByName(const EqPreset& A, const EqPreset& B)
{
    return A.Name < B.Name;
}

} // namespace

void WriteEqCurve(std::ostream& Out, const std::string& Prefix, const std::array<EqBand, MaxEqBands>& Bands,
                  const BassEnhancerSettings& Bass)
{
    for (size_t i = 0; i < MaxEqBands; ++i)
    {
        const EqBand& Band = Bands[i];
        Out << BandKey(Prefix, i, "type") << "=" << static_cast<int>(Band.FilterType) << "\n";
        Out << BandKey(Prefix, i, "freq") << "=" << Band.FrequencyHz << "\n";
        Out << BandKey(Prefix, i, "gain") << "=" << Band.GainDb << "\n";
        Out << BandKey(Prefix, i, "q") << "=" << Band.Q << "\n";
    }
    Out << Prefix << "bass_enhancer.enabled=" << (Bass.bEnabled ? 1 : 0) << "\n";
    Out << Prefix << "bass_enhancer.cutoff_hz=" << Bass.CutoffHz << "\n";
    Out << Prefix << "bass_enhancer.drive=" << Bass.Drive << "\n";
    Out << Prefix << "bass_enhancer.mix=" << Bass.Mix << "\n";
}

void ReadEqCurve(const KeyValueFile& File, const std::string& Prefix, std::array<EqBand, MaxEqBands>& OutBands,
                 BassEnhancerSettings& OutBass)
{
    for (size_t i = 0; i < MaxEqBands; ++i)
    {
        EqBand& Band = OutBands[i];
        const long Type = File.GetLong(BandKey(Prefix, i, "type"), static_cast<long>(Band.FilterType));
        if (Type >= 0 && Type <= static_cast<long>(EqFilterType::Notch))
        {
            Band.FilterType = static_cast<EqFilterType>(Type);
        }
        Band.FrequencyHz = File.GetFloat(BandKey(Prefix, i, "freq"), Band.FrequencyHz);
        Band.GainDb = File.GetFloat(BandKey(Prefix, i, "gain"), Band.GainDb);
        Band.Q = File.GetFloat(BandKey(Prefix, i, "q"), Band.Q);
    }
    OutBass.bEnabled = File.GetBool(Prefix + "bass_enhancer.enabled", OutBass.bEnabled);
    OutBass.CutoffHz = File.GetFloat(Prefix + "bass_enhancer.cutoff_hz", OutBass.CutoffHz);
    OutBass.Drive = File.GetFloat(Prefix + "bass_enhancer.drive", OutBass.Drive);
    OutBass.Mix = File.GetFloat(Prefix + "bass_enhancer.mix", OutBass.Mix);
}

std::vector<EqPreset> BuiltInEqPresets()
{
    BassEnhancerSettings BassOff;
    BassEnhancerSettings BassOn;
    BassOn.bEnabled = true;

    //                                     31.5   63   125  250  500   1k   2k   4k   8k  16k
    return {
        MakeGraphicPreset("Flat",       {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, BassOff),
        MakeGraphicPreset("Bass Boost", {6, 5, 4, 2, 0, 0, 0, 0, 0, 0}, BassOn),
        MakeGraphicPreset("Vocal",      {-3, -2, -1, 0, 1, 3, 3.5f, 2.5f, 0, -1}, BassOff),
    };
}

EqPresetCatalog::EqPresetCatalog(PresetFileContents Contents)
    : BuiltIns(BuiltInEqPresets()), User(std::move(Contents.UserPresets))
{
    for (EqPreset& Preset : User)
    {
        Preset.bBuiltIn = false;
    }
    std::sort(User.begin(), User.end(), ByName);

    for (auto& [Target, Preset] : Contents.DeviceBindings)
    {
        Bind(Devices, Target, Preset);
    }
    for (auto& [Target, Preset] : Contents.AppBindings)
    {
        Bind(Apps, Target, Preset);
    }
}

const EqPreset* EqPresetCatalog::Find(const std::string& Name) const
{
    for (const EqPreset& Preset : BuiltIns)
    {
        if (Preset.Name == Name)
        {
            return &Preset;
        }
    }
    for (const EqPreset& Preset : User)
    {
        if (Preset.Name == Name)
        {
            return &Preset;
        }
    }
    return nullptr;
}

EqPresetCatalog::Result EqPresetCatalog::Save(const std::string& Name, const std::array<EqBand, MaxEqBands>& Bands,
                                              const BassEnhancerSettings& Bass, std::string* OutStoredName)
{
    const std::optional<std::string> Normalized = NormalizePresetName(Name);
    if (!Normalized)
    {
        return Result::InvalidName;
    }
    if (const EqPreset* Existing = Find(*Normalized); Existing && Existing->bBuiltIn)
    {
        return Result::BuiltInReadOnly;
    }

    auto It = std::find_if(User.begin(), User.end(), [&](const EqPreset& P) { return P.Name == *Normalized; });
    if (It == User.end())
    {
        EqPreset NewPreset;
        NewPreset.Name = *Normalized;
        It = User.insert(std::upper_bound(User.begin(), User.end(), NewPreset, ByName), NewPreset);
    }
    It->Bands = Bands;
    It->Bass = Bass;
    if (OutStoredName)
    {
        *OutStoredName = *Normalized;
    }
    return Result::Ok;
}

EqPresetCatalog::Result EqPresetCatalog::Remove(const std::string& Name)
{
    const EqPreset* Existing = Find(Name);
    if (!Existing)
    {
        return Result::NotFound;
    }
    if (Existing->bBuiltIn)
    {
        return Result::BuiltInReadOnly;
    }
    User.erase(std::remove_if(User.begin(), User.end(), [&](const EqPreset& P) { return P.Name == Name; }),
               User.end());
    for (PresetBindingMap* Map : {&Devices, &Apps})
    {
        std::erase_if(*Map, [&](const auto& Entry) { return Entry.second == Name; });
    }
    return Result::Ok;
}

EqPresetCatalog::Result EqPresetCatalog::Bind(PresetBindingMap& Map, const std::string& Target,
                                              const std::string& PresetName)
{
    if (!IsValidTargetName(Target))
    {
        return Result::InvalidName;
    }
    if (PresetName.empty())
    {
        Map.erase(Target);
        return Result::Ok;
    }
    if (!Find(PresetName))
    {
        return Result::NotFound;
    }
    Map[Target] = PresetName;
    return Result::Ok;
}

EqPresetCatalog::Result EqPresetCatalog::BindDevice(const std::string& DeviceName, const std::string& PresetName)
{
    return Bind(Devices, DeviceName, PresetName);
}

EqPresetCatalog::Result EqPresetCatalog::BindApp(const std::string& AppName, const std::string& PresetName)
{
    return Bind(Apps, AppName, PresetName);
}

std::string EqPresetCatalog::DeviceBinding(const std::string& DeviceName) const
{
    const auto It = Devices.find(DeviceName);
    return It != Devices.end() ? It->second : std::string();
}

std::string EqPresetCatalog::AppBinding(const std::string& AppName) const
{
    const auto It = Apps.find(AppName);
    return It != Apps.end() ? It->second : std::string();
}

std::vector<EqPresetInfo> EqPresetCatalog::Infos() const
{
    std::vector<EqPresetInfo> Out;
    Out.reserve(BuiltIns.size() + User.size());
    for (const EqPreset& Preset : BuiltIns)
    {
        Out.push_back({Preset.Name, true});
    }
    for (const EqPreset& Preset : User)
    {
        Out.push_back({Preset.Name, false});
    }
    return Out;
}

PresetFileContents EqPresetCatalog::ToFileContents() const
{
    return {User, Devices, Apps};
}

PresetStore::PresetStore() : Path(DefaultPresetsPath())
{
}

PresetStore::PresetStore(std::string InPath) : Path(std::move(InPath))
{
}

namespace
{
// Bindings are stored as `<Kind>_binding.count` plus
// `<Kind>_binding.<n>.target` / `.preset` pairs - keyed by index rather
// than by target, since device node.names and app names may contain '='.
void WriteBindings(std::ostream& Out, const char* Kind, const PresetBindingMap& Map)
{
    Out << Kind << "_binding.count=" << Map.size() << "\n";
    size_t i = 0;
    for (const auto& [Target, Preset] : Map)
    {
        const std::string Prefix = std::string(Kind) + "_binding." + std::to_string(i++) + ".";
        Out << Prefix << "target=" << Target << "\n";
        Out << Prefix << "preset=" << Preset << "\n";
    }
}

PresetBindingMap ReadBindings(const KeyValueFile& File, const char* Kind)
{
    PresetBindingMap Map;
    const size_t Count = File.GetCount(std::string(Kind) + "_binding.count");
    for (size_t i = 0; i < Count; ++i)
    {
        const std::string Prefix = std::string(Kind) + "_binding." + std::to_string(i) + ".";
        const std::string Target = File.GetString(Prefix + "target", "");
        const std::string Preset = File.GetString(Prefix + "preset", "");
        if (!Target.empty() && !Preset.empty())
        {
            Map[Target] = Preset;
        }
    }
    return Map;
}
} // namespace

PresetFileContents PresetStore::Load() const
{
    const std::optional<KeyValueFile> File = KeyValueFile::Load(Path);
    if (!File)
    {
        return {};
    }

    const std::vector<EqPreset> BuiltIns = BuiltInEqPresets();
    const size_t Count = File->GetCount("preset.count");
    std::vector<EqPreset> Presets;
    for (size_t i = 0; i < Count; ++i)
    {
        const std::string Prefix = "preset." + std::to_string(i) + ".";
        const std::optional<std::string> Name = NormalizePresetName(File->GetString(Prefix + "name", ""));
        const auto SameName = [&](const EqPreset& P) { return Name && P.Name == *Name; };
        if (!Name || std::any_of(BuiltIns.begin(), BuiltIns.end(), SameName) ||
            std::any_of(Presets.begin(), Presets.end(), SameName))
        {
            fprintf(stderr, "[ramkolfxd] presets: skipping entry %zu (invalid, duplicate, or built-in name)\n", i);
            continue;
        }
        EqPreset Preset;
        Preset.Name = *Name;
        Preset.Bands = DefaultHwEqBands();
        ReadEqCurve(*File, Prefix, Preset.Bands, Preset.Bass);
        Presets.push_back(std::move(Preset));
    }
    return {std::move(Presets), ReadBindings(*File, "device"), ReadBindings(*File, "app")};
}

bool PresetStore::Save(const PresetFileContents& Contents) const
{
    const std::vector<EqPreset>& UserPresets = Contents.UserPresets;
    std::ostringstream Out;
    Out << "preset.count=" << UserPresets.size() << "\n";
    for (size_t i = 0; i < UserPresets.size(); ++i)
    {
        const std::string Prefix = "preset." + std::to_string(i) + ".";
        Out << Prefix << "name=" << UserPresets[i].Name << "\n";
        WriteEqCurve(Out, Prefix, UserPresets[i].Bands, UserPresets[i].Bass);
    }
    WriteBindings(Out, "device", Contents.DeviceBindings);
    WriteBindings(Out, "app", Contents.AppBindings);
    return WriteFileAtomically(Path, Out.str());
}

} // namespace ramkolfx
