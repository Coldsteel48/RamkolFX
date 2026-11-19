// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "ramkolfx/key_value_file.hpp"

#include <algorithm>
#include <fstream>

namespace ramkolfx
{

std::optional<KeyValueFile> KeyValueFile::Load(const std::string& Path)
{
    std::ifstream File(Path);
    if (!File.is_open())
    {
        return std::nullopt;
    }

    KeyValueFile Result;
    std::string Line;
    while (std::getline(File, Line))
    {
        const size_t Eq = Line.find('=');
        if (Eq == std::string::npos)
        {
            continue;
        }
        Result.Fields.emplace(Line.substr(0, Eq), Line.substr(Eq + 1));
    }
    return Result;
}

bool KeyValueFile::Has(const std::string& Key) const
{
    return Fields.find(Key) != Fields.end();
}

std::string KeyValueFile::GetString(const std::string& Key, const std::string& Default) const
{
    const auto It = Fields.find(Key);
    return It != Fields.end() ? It->second : Default;
}

long KeyValueFile::GetLong(const std::string& Key, long Default) const
{
    const auto It = Fields.find(Key);
    if (It == Fields.end())
    {
        return Default;
    }
    try
    {
        return std::stol(It->second);
    }
    catch (...)
    {
        return Default;
    }
}

float KeyValueFile::GetFloat(const std::string& Key, float Default) const
{
    const auto It = Fields.find(Key);
    if (It == Fields.end())
    {
        return Default;
    }
    try
    {
        return std::stof(It->second);
    }
    catch (...)
    {
        return Default;
    }
}

bool KeyValueFile::GetBool(const std::string& Key, bool Default) const
{
    const auto It = Fields.find(Key);
    return It != Fields.end() ? It->second == "1" : Default;
}

size_t KeyValueFile::GetCount(const std::string& Key) const
{
    const long Count = GetLong(Key, 0);
    return Count <= 0 ? 0 : std::min(static_cast<size_t>(Count), Fields.size());
}

} // namespace ramkolfx
