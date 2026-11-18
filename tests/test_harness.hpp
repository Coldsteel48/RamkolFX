// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

// Minimal header-only test harness - deliberately not GoogleTest/Catch2,
// so the test build adds no dependency to fetch. Each test file defines
// TEST(...) functions and includes this header once; RAMKOLFX_TEST_MAIN()
// runs them all. CHECK records a failure and keeps going, so one run
// reports every broken expectation in a test, not just the first.
//
//   TEST(SomethingWorks) { CHECK(1 + 1 == 2); }
//   RAMKOLFX_TEST_MAIN()

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace ramkolfx::test
{

struct TestCase
{
    const char* Name;
    void (*Fn)();
};

inline std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> Tests;
    return Tests;
}

inline int& FailureCount()
{
    static int Failures = 0;
    return Failures;
}

struct Registrar
{
    Registrar(const char* Name, void (*Fn)())
    {
        Registry().push_back({Name, Fn});
    }
};

inline int RunAll()
{
    int FailedTests = 0;
    for (const TestCase& Test : Registry())
    {
        const int Before = FailureCount();
        Test.Fn();
        const bool bPassed = FailureCount() == Before;
        FailedTests += bPassed ? 0 : 1;
        std::printf("[%s] %s\n", bPassed ? " OK " : "FAIL", Test.Name);
    }
    std::printf("%zu tests, %d failed\n", Registry().size(), FailedTests);
    return FailedTests == 0 ? 0 : 1;
}

// A unique, not-yet-existing path in the temp directory, removed when the
// object goes out of scope (along with any "<path>.tmp" left behind).
class TempPath
{
public:
    TempPath()
    {
        const char* Dir = std::getenv("TMPDIR");
        std::string Template = std::string(Dir && *Dir ? Dir : "/tmp") + "/ramkolfx_test_XXXXXX";
        const int Fd = mkstemp(Template.data());
        if (Fd >= 0)
        {
            close(Fd);
            unlink(Template.c_str()); // callers expect "doesn't exist yet"
        }
        Path = Template;
    }
    ~TempPath()
    {
        unlink(Path.c_str());
        unlink((Path + ".tmp").c_str());
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;

    const std::string& Get() const
    {
        return Path;
    }

private:
    std::string Path;
};

} // namespace ramkolfx::test

#define TEST(Name)                                                                 \
    static void Name();                                                            \
    static ::ramkolfx::test::Registrar Name##_Registrar(#Name, &Name);             \
    static void Name()

#define CHECK(Cond)                                                                \
    do                                                                             \
    {                                                                              \
        if (!(Cond))                                                               \
        {                                                                          \
            std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #Cond); \
            ++::ramkolfx::test::FailureCount();                                    \
        }                                                                          \
    } while (0)

// CHECK that also returns from the current test on failure - for
// preconditions later checks would dereference (e.g. an optional).
#define REQUIRE(Cond)                                                              \
    do                                                                             \
    {                                                                              \
        if (!(Cond))                                                               \
        {                                                                          \
            std::printf("  %s:%d: REQUIRE(%s) failed\n", __FILE__, __LINE__, #Cond); \
            ++::ramkolfx::test::FailureCount();                                    \
            return;                                                                \
        }                                                                          \
    } while (0)

#define RAMKOLFX_TEST_MAIN()                                                       \
    int main()                                                                     \
    {                                                                              \
        return ::ramkolfx::test::RunAll();                                         \
    }
