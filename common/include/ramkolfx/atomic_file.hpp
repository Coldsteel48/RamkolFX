// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <string>

namespace ramkolfx
{

struct AtomicWriteOptions
{
    // Prefixed to every stderr message, so the log says which process failed.
    const char* LogPrefix = "[ramkolfx]";

    // fsync the temp file before the rename (and the directory after it).
    // Without that, some filesystems can commit the rename before the data
    // and leave an empty file after a power cut - but an fsync can also
    // block for tens to hundreds of ms on a busy or slow disk. Turn it off
    // only for files that are cheap to lose and written from latency-
    // sensitive code (e.g. cosmetic UI state saved from the render loop):
    // the rename still keeps a crash from leaving a half-written file.
    bool bDurable = true;
};

// Writes Contents to Path via write-to-temp-then-rename: rename() is atomic
// within the same directory, so a crash mid-write leaves either the old
// file or the new one intact, never a half-written one. Shared by the
// daemon's settings/preset stores and the GUI's own config files. A
// missing parent directory is created (owner-only, like `mkdir -p`) and
// the write retried, so callers never need to create it themselves.
// Best-effort: logs to stderr, removes the temp file and returns false on
// failure.
bool WriteFileAtomically(const std::string& Path, const std::string& Contents, const AtomicWriteOptions& Options = {});

} // namespace ramkolfx
