// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "desktop_exec.hpp"

#include <string_view>

namespace ramkolfx::gui
{

std::string QuoteDesktopExecArgument(const std::string& Argument)
{
    std::string Quoted = "\"";
    for (const char C : Argument)
    {
        if (C == '"' || C == '`' || C == '$' || C == '\\')
        {
            Quoted += '\\';
        }
        else if (C == '%')
        {
            Quoted += '%';
        }
        Quoted += C;
    }
    Quoted += '"';

    std::string Escaped;
    Escaped.reserve(Quoted.size());
    for (const char C : Quoted)
    {
        switch (C)
        {
        case '\\':
            Escaped += "\\\\";
            break;
        case '\n':
            Escaped += "\\n";
            break;
        case '\t':
            Escaped += "\\t";
            break;
        case '\r':
            Escaped += "\\r";
            break;
        default:
            Escaped += C;
            break;
        }
    }
    return Escaped;
}

std::string DesktopExecProgram(const std::string& ExecValue)
{
    std::string Unescaped;
    for (size_t i = 0; i < ExecValue.size(); ++i)
    {
        const char C = ExecValue[i];
        if (C != '\\' || i + 1 == ExecValue.size())
        {
            Unescaped += C;
            continue;
        }
        switch (const char Next = ExecValue[++i])
        {
        case 's':
            Unescaped += ' ';
            break;
        case 'n':
            Unescaped += '\n';
            break;
        case 't':
            Unescaped += '\t';
            break;
        case 'r':
            Unescaped += '\r';
            break;
        case '\\':
            Unescaped += '\\';
            break;
        default:
            Unescaped += '\\';
            Unescaped += Next;
            break;
        }
    }

    std::string Program;
    size_t i = Unescaped.find_first_not_of(' ');
    if (i != std::string::npos && Unescaped[i] == '"')
    {
        for (++i; i < Unescaped.size() && Unescaped[i] != '"'; ++i)
        {
            constexpr std::string_view QuotedEscapes = "\"`$\\";
            if (Unescaped[i] == '\\' && i + 1 < Unescaped.size() &&
                QuotedEscapes.find(Unescaped[i + 1]) != std::string_view::npos)
            {
                ++i;
            }
            Program += Unescaped[i];
        }
    }
    else if (i != std::string::npos)
    {
        Program = Unescaped.substr(i, Unescaped.find(' ', i) - i);
    }

    for (size_t Percent = Program.find("%%"); Percent != std::string::npos; Percent = Program.find("%%", Percent + 1))
    {
        Program.erase(Percent, 1);
    }
    return Program;
}

} // namespace ramkolfx::gui
