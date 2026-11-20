// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// Finding the ONNX Runtime library to dlopen, whatever the distro calls
// it: Arch/Fedora/Alpine ship libonnxruntime.so.1, Debian only the
// versioned libonnxruntime.so.1.21.

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "detect/onnx_classifier.hpp"
#include "test_harness.hpp"

using namespace ramkolfx;

namespace
{
struct TempDir
{
    std::filesystem::path Path;
    TempDir()
    {
        Path = std::filesystem::temp_directory_path() / ("ramkolfx-ort-lookup-" + std::to_string(getpid()));
        std::filesystem::remove_all(Path);
        std::filesystem::create_directories(Path);
    }
    ~TempDir()
    {
        std::filesystem::remove_all(Path);
    }
    std::string Touch(const std::string& Name) const
    {
        std::ofstream(Path / Name) << "x";
        return (Path / Name).string();
    }
};
} // namespace

TEST(FindsDebiansVersionedLibraryAndSkipsTheProviders)
{
    TempDir Dir;
    const std::string Lib = Dir.Touch("libonnxruntime.so.1.21");
    Dir.Touch("libonnxruntime_providers_shared.so");
    Dir.Touch("libonnxruntime_providers_dnnl.so");
    Dir.Touch("libonnxruntime.so.1.21.tmp");
    const std::vector<std::string> Found = FindOnnxRuntimeLibraries({Dir.Path.string()}, "");
    REQUIRE(Found.size() == 1);
    CHECK(Found[0] == Lib);
}

TEST(NewestVersionComesFirst)
{
    TempDir Dir;
    const std::string Old = Dir.Touch("libonnxruntime.so.1.9.0");
    const std::string New = Dir.Touch("libonnxruntime.so.1.21.0");
    const std::string Plain = Dir.Touch("libonnxruntime.so");
    const std::vector<std::string> Found = FindOnnxRuntimeLibraries({Dir.Path.string()}, "");
    REQUIRE(Found.size() == 3);
    CHECK(Found[0] == New); // 1.21 > 1.9 numerically, not as text
    CHECK(Found[1] == Old);
    CHECK(Found[2] == Plain);
}

TEST(NamesFromTheLinkerCache)
{
    // ld.so.cache: NUL-separated names and paths.
    const std::string Cache = std::string("junk\0libonnxruntime.so.1.23\0/usr/lib/libonnxruntime.so.1.23\0", 60) +
                              std::string("libonnxruntime_providers_shared.so\0libfoo.so.1\0", 46);
    const std::vector<std::string> Found = FindOnnxRuntimeLibraries({}, Cache);
    REQUIRE(Found.size() == 1);
    CHECK(Found[0] == "libonnxruntime.so.1.23");
}

TEST(MissingDirectoriesAreFine)
{
    CHECK(FindOnnxRuntimeLibraries({"/nonexistent/ramkolfx"}, "").empty());
}

RAMKOLFX_TEST_MAIN()
