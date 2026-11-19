// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <string>

namespace ramkolfx::gui
{

// The Exec= key of a Desktop Entry file, as far as the autostart entry
// needs it (see autostart.hpp). Kept apart, with no dependencies, so the
// unit tests can check that the two stay each other's inverse.

// Quotes one Exec= argument per the Desktop Entry spec, which layers two
// escaping schemes: inside double quotes, the characters " ` $ and
// backslash take a backslash (and a literal % is written %% so it isn't
// read as a field code); the result is then a string-typed value, whose
// own escapes (backslash, newline, tab, CR) apply on top - which is why
// one literal backslash in the path ends up as four in the file.
std::string QuoteDesktopExecArgument(const std::string& Argument);

// The program (first argument) of an Exec= value, with the escaping that
// QuoteDesktopExecArgument applies undone in reverse: string-value escapes
// first, then the quoting rule, then %%. An unquoted program ends at the
// first space.
std::string DesktopExecProgram(const std::string& ExecValue);

} // namespace ramkolfx::gui
