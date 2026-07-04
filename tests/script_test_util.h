// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Shared test-only helpers for the headless ScriptDataSource tests
// (test_script_trigger.cpp, test_script_http.cpp) — not part of the engine.

#pragma once

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace script_test_util {

inline int g_failures = 0;

inline void Check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

// ScriptDataSource runs Lua on its own worker thread; any assertion that
// depends on the worker having processed a job or completed a fetch must
// poll instead of checking immediately. `pred` is called on every attempt,
// so it can (and usually should) call ds.GetData() itself — that's the only
// thing that drains a pending script -> host trigger and applies it to Title.
template <typename Pred>
bool PollUntil(
    Pred&& pred,
    std::chrono::milliseconds timeout = std::chrono::seconds(2),
    std::chrono::milliseconds interval = std::chrono::milliseconds(5)
)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (pred()) return true;
        std::this_thread::sleep_for(interval);
    } while (std::chrono::steady_clock::now() < deadline);
    return pred();
}

inline std::string WriteScript(const std::string& name, const std::string& content)
{
    std::string path = (std::filesystem::temp_directory_path() / ("ogt-script-test-" + name + ".lua")).string();
    std::ofstream f(path, std::ios::trunc);
    f << content;
    return path;
}

}  // namespace script_test_util
