// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test: GetDataBlocking() must fail fast once a script's
// worker has given up loading, instead of burning the full
// kInstantFetchTimeout on every call — see script.cpp's WorkerMain catch
// block and GetDataBlocking's m_loadFailed fast path.

#include "script.h"
#include "script_test_util.h"

#include <chrono>
#include <vector>

using script_test_util::Check;
using script_test_util::g_failures;
using script_test_util::PollUntil;
using script_test_util::WriteScript;

int main()
{
    // Deliberately unparsable, so safe_script_file throws a sol::error and
    // WorkerMain's catch block runs, setting m_loadFailed and exiting the
    // worker thread for good.
    std::string path = WriteScript("load_failure", "this is not lua\n");

    ScriptDataSource ds(path);

    bool failed = PollUntil([&] { return ds.LoadFailed(); }, std::chrono::seconds(2));
    Check(failed, "worker reports LoadFailed() after a bad script");
    Check(!ds.GetLoadError().empty(), "GetLoadError() is non-empty");

    auto start = std::chrono::steady_clock::now();
    std::vector<Record> recs = ds.GetDataBlocking();
    auto elapsed = std::chrono::steady_clock::now() - start;

    Check(recs.empty(), "GetDataBlocking() returns the (empty) cache");
    Check(elapsed < std::chrono::seconds(1),
          "GetDataBlocking() returns near-instantly instead of the full kInstantFetchTimeout");

    if (g_failures == 0) {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
