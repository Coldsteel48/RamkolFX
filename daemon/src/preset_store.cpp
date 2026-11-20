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

#include "dsp/graphic_eq_filter.hpp"
#include "ramkolfx/key_value_file.hpp"
#include "ramkolfx/atomic_file.hpp"
#include "ramkolfx/xdg_paths.hpp"

namespace ramkolfx
{

namespace
{

std::string DefaultPresetsPath()
{
    return RamkolfxConfigDir() + "/presets.conf";
}

std::string BandKey(const std::string& Prefix, size_t Index, const char* Field)
{
    return Prefix + "hweq." + std::to_string(Index) + "." + Field;
}

// Builds a built-in preset from per-band gains on the default ISO graphic
// EQ curve (same frequencies/Q the GUI's simple-mode sliders use), so
// every built-in is reproducible by hand in simple mode.
EqPreset MakeGraphicPreset(EqPresetCategory Category, const char* Name, const std::array<float, MaxEqBands>& GainsDb,
                           const BassEnhancerSettings& Bass = {})
{
    EqPreset Preset;
    Preset.Name = Name;
    Preset.Bands = DefaultHwEqBands();
    for (size_t i = 0; i < MaxEqBands; ++i)
    {
        Preset.Bands[i].GainDb = GainsDb[i];
    }
    Preset.Bass = Bass;
    Preset.Category = Category;
    return Preset;
}

BassEnhancerSettings BassEnhancerOn(float CutoffHz, float Drive, float Mix)
{
    return {.bEnabled = true, .CutoffHz = CutoffHz, .Drive = Drive, .Mix = Mix};
}

bool ByName(const EqPreset& A, const EqPreset& B)
{
    return A.Name < B.Name;
}

std::vector<EqPreset> MakeBuiltInEqPresets()
{
    // There is no preamp or limiter after the EQ, so these lean on cuts
    // and keep boosts moderate: each curve's summed response (overlapping
    // bands add up) peaks at +6 dB at most - preset_store_tests measures
    // it on the real filter. The bass enhancer adds harmonics on top of
    // that rather than low-end level, so it's the gentler way to add
    // weight, but it isn't free headroom either.
    constexpr auto General = EqPresetCategory::General;
    constexpr auto Music = EqPresetCategory::Music;
    constexpr auto Games = EqPresetCategory::Games;

    // clang-format off
    //                                                   31.5  63   125   250   500   1k    2k    4k    8k    16k
    return {
        MakeGraphicPreset(General, "Flat",         {0,    0,    0,    0,    0,    0,    0,    0,    0,    0}),
        MakeGraphicPreset(General, "Bass Boost",   {5,    4,    3,    1.5f, 0,    0,    0,    0,    0,    0},
                          BassEnhancerOn(60, 0.5f, 0.5f)),
        MakeGraphicPreset(General, "Treble Boost", {0,    0,    0,    0,    0,    0,    1,    2.5f, 4,    4}),
        MakeGraphicPreset(General, "Vocal",        {-3,   -2,   -1,   0,    1,    3,    3.5f, 2.5f, 0,    -1}),
        // Low-volume listening: lifts the lows and highs the ear loses first.
        MakeGraphicPreset(General, "Loudness",     {4,    3,    1.5f, 0,    -1,   -1,   0,    1,    2.5f, 3}),
        // Tames booms so the volume can stay down, keeps dialogue clear.
        MakeGraphicPreset(General, "Late Night",   {-5,   -4,   -2,   0,    0,    1,    2,    1.5f, 0,    -1}),
        // Podcasts, audiobooks, calls: rumble out, speech presence up.
        MakeGraphicPreset(General, "Spoken Word",  {-6,   -4,   -1.5f, 0,   1,    2,    3,    2,    0,    -2}),
        MakeGraphicPreset(General, "Movie",        {3,    3,    1.5f, 0,    -1,   0.5f, 2,    2,    1.5f, 1},
                          BassEnhancerOn(80, 0.4f, 0.4f)),

        MakeGraphicPreset(Music,   "Acoustic",     {1,    1,    1.5f, 1,    0,    0,    1,    2,    2,    1.5f}),
        MakeGraphicPreset(Music,   "Blues",        {2,    2,    1.5f, 1,    0,    -0.5f, 1,   2,    1,    0}),
        MakeGraphicPreset(Music,   "Classical",    {2.5f, 2,    1,    0,    0,    0,    0,    0.5f, 1.5f, 2}),
        MakeGraphicPreset(Music,   "Electronic",   {5,    4,    1.5f, -1,   -2,   0,    0.5f, 2,    3,    3.5f},
                          BassEnhancerOn(70, 0.6f, 0.5f)),
        MakeGraphicPreset(Music,   "Hip-Hop",      {5,    4.5f, 2,    0,    -1.5f, -0.5f, 1,  1,    1.5f, 1},
                          BassEnhancerOn(60, 0.5f, 0.5f)),
        MakeGraphicPreset(Music,   "Jazz",         {2,    1.5f, 1,    1.5f, -1,   -1,   0,    1,    2,    2.5f}),
        MakeGraphicPreset(Music,   "Metal",        {4,    3,    0,    -2,   -3,   -1,   2,    3.5f, 3,    1.5f}),
        MakeGraphicPreset(Music,   "Pop",          {1,    2,    1.5f, 0,    -1,   0.5f, 2,    2.5f, 2,    1.5f}),
        MakeGraphicPreset(Music,   "R&B",          {4,    4,    2,    -1,   -1.5f, 1,   2,    1.5f, 1.5f, 1},
                          BassEnhancerOn(60, 0.4f, 0.4f)),
        MakeGraphicPreset(Music,   "Reggae",       {4,    4,    2,    0,    -1.5f, -1,  0,    1,    0.5f, 0},
                          BassEnhancerOn(70, 0.4f, 0.4f)),
        MakeGraphicPreset(Music,   "Rock",         {4,    3,    1.5f, -1,   -2,   -0.5f, 1.5f, 3,   3.5f, 3}),

        MakeGraphicPreset(Games,   "Action",       {4,    4,    2,    0,    -1,   0,    1.5f, 2.5f, 2,    1.5f},
                          BassEnhancerOn(70, 0.5f, 0.5f)),
        // Footsteps and positional cues up, explosions and rumble down.
        MakeGraphicPreset(Games,   "FPS",          {-4,   -3,   -1.5f, 0,   0,    1,    2.5f, 4,    3,    1}),
        // Quiet detail and ambience forward.
        MakeGraphicPreset(Games,   "Horror",       {2,    1.5f, 0,    -1,   -1,   0,    1.5f, 3,    3.5f, 3}),
        // Engine growl and tyre noise.
        MakeGraphicPreset(Games,   "Racing",       {3,    3,    2.5f, 1,    -1,   0,    1,    2,    1.5f, 1},
                          BassEnhancerOn(80, 0.4f, 0.4f)),
        // Orchestral score plus dialogue.
        MakeGraphicPreset(Games,   "RPG",          {3,    2,    1,    0,    -0.5f, 1,   2,    1.5f, 1.5f, 2}),
        // Unit voices and alerts over a busy battle mix.
        MakeGraphicPreset(Games,   "RTS",          {-2,   -1,   0,    0,    0.5f, 2,    2.5f, 1.5f, 0.5f, 0}),
    };
    // clang-format on
}

// The built-ins of every release before settings.conf recorded whether
// the active preset was one (eq_preset.active_builtin). Frozen: it says
// what an old file's name meant, not what this build ships.
bool WasBuiltInBeforeFlagExisted(const std::string& Name)
{
    return Name == "Flat" || Name == "Bass Boost" || Name == "Vocal";
}

// Gives every user preset whose name a built-in now has (one saved
// before that built-in shipped) a free "<name> (custom)" name instead.
// Runs once all names are known, so a new name can't collide with a
// preset further down. Returns old name -> new name.
std::map<std::string, std::string> RenameShadowedUserPresets(std::vector<EqPreset>& Presets)
{
    std::map<std::string, std::string> Renamed;
    const auto IsTaken = [&](const std::string& Name)
    {
        return FindBuiltInEqPreset(Name) ||
               std::any_of(Presets.begin(), Presets.end(), [&](const EqPreset& P) { return P.Name == Name; });
    };
    for (auto It = Presets.begin(); It != Presets.end();)
    {
        if (!FindBuiltInEqPreset(It->Name))
        {
            ++It;
            continue;
        }
        std::optional<std::string> NewName;
        for (int Suffix = 1; !NewName || IsTaken(*NewName); ++Suffix)
        {
            NewName = NormalizePresetName(It->Name + (Suffix == 1 ? " (custom)"
                                                                  : " (custom " + std::to_string(Suffix) + ")"));
            if (!NewName)
            {
                break; // too long to suffix - can't happen with the built-in names, but never loop forever
            }
        }
        if (!NewName)
        {
            fprintf(stderr, "[ramkolfxd] presets: skipping \"%s\" (name taken by a built-in)\n", It->Name.c_str());
            It = Presets.erase(It);
            continue;
        }
        fprintf(stderr, "[ramkolfxd] presets: \"%s\" is now a built-in; kept yours as \"%s\"\n", It->Name.c_str(),
                NewName->c_str());
        Renamed[It->Name] = *NewName;
        It->Name = std::move(*NewName);
        ++It;
    }
    return Renamed;
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

const std::vector<EqPreset>& BuiltInEqPresets()
{
    static const std::vector<EqPreset> BuiltIns = MakeBuiltInEqPresets();
    return BuiltIns;
}

const EqPreset* FindBuiltInEqPreset(const std::string& Name)
{
    for (const EqPreset& Preset : BuiltInEqPresets())
    {
        if (Preset.Name == Name)
        {
            return &Preset;
        }
    }
    return nullptr;
}

const std::string& DefaultDetectedClassPreset(DetectedClass Class)
{
    // Indexed by DetectedClass. Every non-empty entry is a built-in, so
    // the defaults can never dangle.
    static const std::array<std::string, DetectedClassCount> Defaults = []
    {
        std::array<std::string, DetectedClassCount> Table;
        Table[static_cast<size_t>(DetectedClass::Speech)] = "Spoken Word";
        Table[static_cast<size_t>(DetectedClass::Movie)] = "Movie";
        for (size_t i = static_cast<size_t>(DetectedClass::Rock); i < DetectedClassCount; ++i)
        {
            Table[i] = DetectedClassLabel(static_cast<DetectedClass>(i)); // genre labels are the built-ins' names
        }
        return Table;
    }();
    static const std::string None;
    const size_t Index = static_cast<size_t>(Class);
    return Index < Defaults.size() ? Defaults[Index] : None;
}

EqPresetCatalog::EqPresetCatalog(PresetFileContents Contents) : User(std::move(Contents.UserPresets))
{
    for (EqPreset& Preset : User)
    {
        Preset.Category = EqPresetCategory::User;
    }
    Renamed = RenameShadowedUserPresets(User);
    std::sort(User.begin(), User.end(), ByName);

    for (PresetBindingMap* Map : {&Contents.DeviceBindings, &Contents.AppBindings})
    {
        for (auto& [Target, Preset] : *Map)
        {
            if (const auto It = Renamed.find(Preset); It != Renamed.end())
            {
                Preset = It->second;
            }
            Bind(Map == &Contents.DeviceBindings ? Devices : Apps,
                 Map == &Contents.DeviceBindings ? AutoDevices : AutoApps, Target, Preset);
        }
    }
    // A target in both (a hand-edited file): the preset binding wins.
    for (const std::string& Target : Contents.AutoDetectDevices)
    {
        if (!Devices.contains(Target))
        {
            BindAuto(Devices, AutoDevices, Target);
        }
    }
    for (const std::string& Target : Contents.AutoDetectApps)
    {
        if (!Apps.contains(Target))
        {
            BindAuto(Apps, AutoApps, Target);
        }
    }

    for (auto& [Class, Preset] : Contents.DetectedClassOverrides)
    {
        if (const auto It = Renamed.find(Preset); It != Renamed.end())
        {
            Preset = It->second;
        }
        // A preset that's gone means "no switch", same as removing it
        // would have.
        if (BindDetectedClass(Class, Preset) == Result::NotFound)
        {
            BindDetectedClass(Class, "");
        }
    }
}

const EqPreset* EqPresetCatalog::Find(const std::string& Name) const
{
    if (const EqPreset* BuiltIn = FindBuiltInEqPreset(Name))
    {
        return BuiltIn;
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

std::string EqPresetCatalog::ResolveStoredName(const std::string& Name, std::optional<bool> bStoredAsBuiltIn) const
{
    if (Name.empty())
    {
        return {};
    }
    // A file from before the flag: back then any name but the three old
    // built-ins was a user preset's.
    const bool bWasBuiltIn = bStoredAsBuiltIn.value_or(WasBuiltInBeforeFlagExisted(Name));
    std::string Current = Name;
    if (!bWasBuiltIn)
    {
        if (const auto It = Renamed.find(Name); It != Renamed.end())
        {
            Current = It->second;
        }
    }
    // Same kind as when stored, or it's not the same preset: a user
    // preset's name that a built-in has since taken (and presets.conf
    // didn't say so - say it was missing) must not pick up the built-in.
    const EqPreset* Preset = Find(Current);
    return Preset && Preset->IsBuiltIn() == bWasBuiltIn ? Current : std::string();
}

EqPresetCatalog::Result EqPresetCatalog::Save(const std::string& Name, const std::array<EqBand, MaxEqBands>& Bands,
                                              const BassEnhancerSettings& Bass, std::string* OutStoredName)
{
    const std::optional<std::string> Normalized = NormalizePresetName(Name);
    if (!Normalized)
    {
        return Result::InvalidName;
    }
    if (const EqPreset* Existing = Find(*Normalized); Existing && Existing->IsBuiltIn())
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
    if (Existing->IsBuiltIn())
    {
        return Result::BuiltInReadOnly;
    }
    User.erase(std::remove_if(User.begin(), User.end(), [&](const EqPreset& P) { return P.Name == Name; }),
               User.end());
    for (PresetBindingMap* Map : {&Devices, &Apps})
    {
        std::erase_if(*Map, [&](const auto& Entry) { return Entry.second == Name; });
    }
    for (size_t i = 1; i < DetectedClassCount; ++i)
    {
        const DetectedClass Class = static_cast<DetectedClass>(i);
        if (DetectedClassPreset(Class) == Name)
        {
            BindDetectedClass(Class, "");
        }
    }
    return Result::Ok;
}

EqPresetCatalog::Result EqPresetCatalog::Bind(PresetBindingMap& Map, AutoDetectTargets& Auto,
                                              const std::string& Target, const std::string& PresetName)
{
    if (!IsValidTargetName(Target))
    {
        return Result::InvalidName;
    }
    if (PresetName.empty())
    {
        Map.erase(Target);
        Auto.erase(Target);
        return Result::Ok;
    }
    if (!Find(PresetName))
    {
        return Result::NotFound;
    }
    Map[Target] = PresetName;
    Auto.erase(Target);
    return Result::Ok;
}

EqPresetCatalog::Result EqPresetCatalog::BindAuto(PresetBindingMap& Map, AutoDetectTargets& Auto,
                                                  const std::string& Target)
{
    if (!IsValidTargetName(Target))
    {
        return Result::InvalidName;
    }
    Map.erase(Target);
    Auto.insert(Target);
    return Result::Ok;
}

EqPresetCatalog::Result EqPresetCatalog::BindDevice(const std::string& DeviceName, const std::string& PresetName)
{
    return Bind(Devices, AutoDevices, DeviceName, PresetName);
}

EqPresetCatalog::Result EqPresetCatalog::BindApp(const std::string& AppName, const std::string& PresetName)
{
    return Bind(Apps, AutoApps, AppName, PresetName);
}

EqPresetCatalog::Result EqPresetCatalog::BindDeviceAutoDetect(const std::string& DeviceName)
{
    return BindAuto(Devices, AutoDevices, DeviceName);
}

EqPresetCatalog::Result EqPresetCatalog::BindAppAutoDetect(const std::string& AppName)
{
    return BindAuto(Apps, AutoApps, AppName);
}

std::string EqPresetCatalog::DetectedClassPreset(DetectedClass Class) const
{
    const auto It = ClassOverrides.find(Class);
    return It != ClassOverrides.end() ? It->second : DefaultDetectedClassPreset(Class);
}

EqPresetCatalog::Result EqPresetCatalog::BindDetectedClass(DetectedClass Class, const std::string& PresetName)
{
    if (Class == DetectedClass::None || static_cast<size_t>(Class) >= DetectedClassCount)
    {
        return Result::InvalidName;
    }
    if (!PresetName.empty() && !Find(PresetName))
    {
        return Result::NotFound;
    }
    if (PresetName == DefaultDetectedClassPreset(Class))
    {
        ClassOverrides.erase(Class); // back to the default: follows the default if a later build changes it
    }
    else
    {
        ClassOverrides[Class] = PresetName;
    }
    return Result::Ok;
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
    const std::vector<EqPreset>& BuiltIns = BuiltInEqPresets();
    Out.reserve(BuiltIns.size() + User.size());
    for (const std::vector<EqPreset>* List : {&BuiltIns, &User})
    {
        for (const EqPreset& Preset : *List)
        {
            Out.push_back({Preset.Name, Preset.Category});
        }
    }
    return Out;
}

PresetFileContents EqPresetCatalog::ToFileContents() const
{
    return {User, Devices, Apps, AutoDevices, AutoApps, ClassOverrides};
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

// Auto-detect targets: `<Kind>_auto.count` plus `<Kind>_auto.<n>=<target>`.
void WriteAutoTargets(std::ostream& Out, const char* Kind, const AutoDetectTargets& Targets)
{
    Out << Kind << "_auto.count=" << Targets.size() << "\n";
    size_t i = 0;
    for (const std::string& Target : Targets)
    {
        Out << Kind << "_auto." << i++ << "=" << Target << "\n";
    }
}

AutoDetectTargets ReadAutoTargets(const KeyValueFile& File, const char* Kind)
{
    AutoDetectTargets Targets;
    const size_t Count = File.GetCount(std::string(Kind) + "_auto.count");
    for (size_t i = 0; i < Count; ++i)
    {
        std::string Target = File.GetString(std::string(Kind) + "_auto." + std::to_string(i), "");
        if (!Target.empty())
        {
            Targets.insert(std::move(Target));
        }
    }
    return Targets;
}

// Class-table overrides: `detected.<class key>=<preset>`, empty value =
// no switch. Keyed by the class's stable id, so a class a newer build
// added (unknown here) is skipped rather than misread.
void WriteClassOverrides(std::ostream& Out, const DetectedClassTable& Overrides)
{
    for (const auto& [Class, Preset] : Overrides)
    {
        if (const char* Key = DetectedClassKey(Class))
        {
            Out << "detected." << Key << "=" << Preset << "\n";
        }
    }
}

DetectedClassTable ReadClassOverrides(const KeyValueFile& File)
{
    DetectedClassTable Overrides;
    for (size_t i = 1; i < DetectedClassCount; ++i)
    {
        const DetectedClass Class = static_cast<DetectedClass>(i);
        const std::string Key = std::string("detected.") + DetectedClassKey(Class);
        if (File.Has(Key))
        {
            Overrides[Class] = File.GetString(Key, "");
        }
    }
    return Overrides;
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

    PresetFileContents Contents;
    std::vector<EqPreset>& Presets = Contents.UserPresets;
    const size_t Count = File->GetCount("preset.count");
    for (size_t i = 0; i < Count; ++i)
    {
        const std::string Prefix = "preset." + std::to_string(i) + ".";
        const std::optional<std::string> Name = NormalizePresetName(File->GetString(Prefix + "name", ""));
        if (!Name || std::any_of(Presets.begin(), Presets.end(), [&](const EqPreset& P) { return P.Name == *Name; }))
        {
            fprintf(stderr, "[ramkolfxd] presets: skipping entry %zu (invalid or duplicate name)\n", i);
            continue;
        }
        EqPreset Preset;
        Preset.Name = *Name;
        Preset.Bands = DefaultHwEqBands();
        ReadEqCurve(*File, Prefix, Preset.Bands, Preset.Bass);
        Presets.push_back(std::move(Preset));
    }

    Contents.DeviceBindings = ReadBindings(*File, "device");
    Contents.AppBindings = ReadBindings(*File, "app");
    Contents.AutoDetectDevices = ReadAutoTargets(*File, "device");
    Contents.AutoDetectApps = ReadAutoTargets(*File, "app");
    Contents.DetectedClassOverrides = ReadClassOverrides(*File);
    return Contents;
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
    WriteAutoTargets(Out, "device", Contents.AutoDetectDevices);
    WriteAutoTargets(Out, "app", Contents.AutoDetectApps);
    WriteClassOverrides(Out, Contents.DetectedClassOverrides);
    return WriteFileAtomically(Path, Out.str(), {.LogPrefix = "[ramkolfxd]"});
}

} // namespace ramkolfx
