// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test (no SDL, no rendering, no Lua): regression coverage
// for a code review of DataPool (data-pool.h/cpp) that found five defects.
// Fake IDataSource implementations stand in for a slow/throwing/scripted
// source so none of this needs ScriptDataSource or a real Lua script.
//
//  - Defect A: Add() primes the cache with an initial fetch, so Data(key)
//    doesn't return {} until some bound Title goes Visible.
//  - Defect B: Remove()/Add()/Clear() must not destroy a source while
//    holding the pool mutex -- a slow ~IDataSource must never stall a
//    concurrent Tick() on another thread.
//  - Defect C: a throwing GetData() must not escape Tick()/DataBlocking()
//    (which would std::terminate on the host's render thread); the previous
//    cache is kept on failure instead of being clobbered with {}.
//  - Defect D: repeated Bind()/Unbind() cycles on the same Title must not
//    grow its onTriggerIn/onTriggerOut vectors without bound.
//  - Defect E: a single Tick() drain must de-duplicate requested trigger_out
//    targets and must not replay TriggerOut() on an already-Hidden/
//    AnimatingOut Title.

#include "data-pool.h"
#include "title.h"
#include "script_test_util.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using script_test_util::Check;
using script_test_util::g_failures;

namespace {

// ── Fakes ────────────────────────────────────────────────────────────────

// Defect B: a destructor slow enough that, if it ever ran under DataPool's
// own mutex, a concurrent Tick() call would visibly stall.
struct SlowDtorSource : IDataSource {
    std::chrono::milliseconds sleepFor;
    explicit SlowDtorSource(std::chrono::milliseconds s) : sleepFor(s) {}
    ~SlowDtorSource() override { std::this_thread::sleep_for(sleepFor); }
    std::vector<Record> GetData() const override { return {}; }
    std::string GetFilePath() const override { return ""; }
};

// Defect A/C: returns good data until told to throw instead, counting calls.
struct FlakyDataSource : IDataSource {
    mutable int calls = 0;
    bool shouldThrow = false;
    std::vector<Record> GetData() const override
    {
        ++calls;
        if (shouldThrow)
            throw std::runtime_error("simulated read failure");
        return { Record{{"value", "good"}} };
    }
    std::string GetFilePath() const override { return ""; }
};

// Defect D: counts NotifyTriggerIn/Out calls so the redirect-cell mechanism
// can be checked for correctness, not just bounded vector size.
struct NotifyCountingSource : IDataSource {
    int inCount = 0, outCount = 0;
    std::vector<Record> GetData() const override { return {}; }
    std::string GetFilePath() const override { return ""; }
    void NotifyTriggerIn(const std::string&, size_t, double) override { ++inCount; }
    void NotifyTriggerOut(const std::string&) override { ++outCount; }
};

// Defect E: lets the test control exactly what DrainOutTriggerRequests()
// returns on the next Tick(), like a script queuing trigger_out() calls.
struct ManualDrainSource : IDataSource {
    std::vector<std::string> pending;
    std::vector<Record> GetData() const override { return {}; }
    std::string GetFilePath() const override { return ""; }
    std::vector<std::string> DrainOutTriggerRequests() override
    {
        auto out = std::move(pending);
        pending.clear();
        return out;
    }
};

// ── Defect A ─────────────────────────────────────────────────────────────

void TestDefectA_AddPrimesCache()
{
    DataPool pool;
    auto srcOwned = std::make_unique<FlakyDataSource>();
    FlakyDataSource* src = srcOwned.get();
    pool.Add("a", std::move(srcOwned));

    Check(src->calls == 1, "DefectA: Add() fetched once to prime the cache");
    auto cached = pool.Data("a");
    Check(!cached.empty() && cached[0].at("value") == "good",
          "DefectA: Data(key) sees real records immediately after Add(), no Title needed");
}

// ── Defect B ─────────────────────────────────────────────────────────────

void TestDefectB_RemoveDoesNotBlockTick()
{
    DataPool pool;
    pool.Add("slow", std::make_unique<SlowDtorSource>(std::chrono::milliseconds(400)));

    std::atomic<bool> removeDone{false};
    std::thread remover([&] {
        pool.Remove("slow");
        removeDone.store(true, std::memory_order_release);
    });

    // While `remover` is presumably blocked inside ~SlowDtorSource, Tick()
    // on this thread must keep returning promptly -- if Remove() destroyed
    // the source under m_mutex, one of these Tick() calls would stall for
    // ~400ms waiting on that same mutex.
    long worstCaseMs = 0;
    int iterations = 0;
    while (!removeDone.load(std::memory_order_acquire) && iterations < 20000) {
        auto tickStart = std::chrono::steady_clock::now();
        pool.Tick(0.01f);
        auto tickMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - tickStart)
                          .count();
        if (tickMs > worstCaseMs) worstCaseMs = tickMs;
        ++iterations;
    }
    remover.join();

    Check(iterations > 0, "DefectB: at least one Tick() ran concurrently with Remove()'s slow destructor");
    Check(worstCaseMs < 200, "DefectB: no single Tick() call stalled anywhere near the destructor's 400ms sleep");
}

// ── Defect C ─────────────────────────────────────────────────────────────

void TestDefectC_ThrowingGetDataDoesNotCrashOrClobberCache()
{
    DataPool pool;
    auto srcOwned = std::make_unique<FlakyDataSource>();
    FlakyDataSource* src = srcOwned.get();
    pool.Add("c", std::move(srcOwned));  // defect A: primes with one good fetch

    Title title;
    pool.Bind(&title, "c", "c");

    title.TriggerIn();
    title.Tick(0.01f);
    Check(title.state == TitleState::Visible, "DefectC setup: title reached Visible");

    // Cross one 0.25s cadence boundary while the source still returns good
    // data -- cache should refresh to it.
    for (int i = 0; i < 30; ++i) {
        title.Tick(0.01f);
        pool.Tick(0.01f);
    }
    Check(src->calls >= 2, "DefectC setup: at least one periodic poll happened");
    auto goodCache = pool.Data("c");
    Check(!goodCache.empty() && goodCache[0].at("value") == "good",
          "DefectC setup: cache holds the last good fetch");

    // Now make every future GetData() throw and cross another cadence
    // boundary. This must not crash the process (if it did, ctest would
    // report this binary as failed/crashed rather than reporting a normal
    // FAIL line) and must not blank the cache.
    src->shouldThrow = true;
    for (int i = 0; i < 30; ++i) {
        title.Tick(0.01f);
        pool.Tick(0.01f);
    }
    auto afterThrow = pool.Data("c");
    Check(!afterThrow.empty() && afterThrow[0].at("value") == "good",
          "DefectC: a throwing GetData() keeps the previous cache instead of clobbering it with {}");
    Check(title.state == TitleState::Visible,
          "DefectC: the title is still fine (process didn't crash, title still Visible)");
}

// ── Defect D ─────────────────────────────────────────────────────────────

void TestDefectD_BoundedSubscribersAcrossBindUnbindCycles()
{
    DataPool pool;
    auto srcOwned = std::make_unique<NotifyCountingSource>();
    NotifyCountingSource* src = srcOwned.get();
    pool.Add("d", std::move(srcOwned));

    Title title;

    constexpr int kCycles = 50;
    for (int i = 0; i < kCycles; ++i) {
        pool.Bind(&title, "d", "n");
        pool.Unbind(&title);
    }

    Check(title.onTriggerIn.size() <= 1,
          "DefectD: onTriggerIn stays bounded (<=1) after repeated Bind/Unbind cycles");
    Check(title.onTriggerOut.size() <= 1,
          "DefectD: onTriggerOut stays bounded (<=1) after repeated Bind/Unbind cycles");

    // Functionality check, not just size: after all that churn, one final
    // Bind() must still correctly relay into the currently-bound source.
    pool.Bind(&title, "d", "n");
    title.TriggerIn();
    Check(src->inCount == 1, "DefectD: the single subscriber still relays NotifyTriggerIn after many cycles");
    title.TriggerOut();
    Check(src->outCount == 1, "DefectD: the single subscriber still relays NotifyTriggerOut after many cycles");
}

// ── Defect E ─────────────────────────────────────────────────────────────

void TestDefectE_DedupAndStateGuard()
{
    DataPool pool;
    auto srcOwned = std::make_unique<ManualDrainSource>();
    ManualDrainSource* src = srcOwned.get();
    pool.Add("e", std::move(srcOwned));

    Title title;
    int outCount = 0;
    title.onTriggerOut.push_back([&] { ++outCount; });
    pool.Bind(&title, "e", "lower");

    title.TriggerIn();
    title.Tick(0.01f);
    Check(title.state == TitleState::Visible, "DefectE setup: title reached Visible");

    // A script whose _get_data calls both trigger_out("lower") and a bare
    // trigger_out() in the same pass drains two entries that both match the
    // "lower" binding -- must fire TriggerOut() exactly once.
    src->pending = {"lower", ""};
    pool.Tick(0.01f);
    Check(outCount == 1, "DefectE: duplicate drained names fire TriggerOut() exactly once");
    Check(title.state == TitleState::AnimatingOut, "DefectE: title moved to AnimatingOut");
    // Exactly two subscribers total: the host-UI-style listener registered
    // above, plus the single DataPool-installed one (defect D holds -- it
    // didn't grow from the Bind() call or from this Tick()'s drain).
    Check(title.onTriggerOut.size() == 2, "DefectE sanity: exactly host-listener + one DataPool subscriber (defect D holds)");

    // Drive it all the way to Hidden.
    float t = 0.0f;
    while (title.state == TitleState::AnimatingOut && t < 5.0f) {
        title.Tick(0.02f);
        t += 0.02f;
    }
    Check(title.state == TitleState::Hidden, "DefectE setup: title reached Hidden");

    // A bare trigger_out() while already Hidden must be a no-op: no replayed
    // out-animation, no extra subscriber firing.
    int outCountBefore = outCount;
    src->pending = {""};
    pool.Tick(0.01f);
    Check(outCount == outCountBefore, "DefectE: trigger_out() on an already-Hidden title does not re-fire TriggerOut()");
    Check(title.state == TitleState::Hidden, "DefectE: already-Hidden title stays Hidden (no replayed out-animation)");
}

}  // namespace

int main()
{
    TestDefectA_AddPrimesCache();
    TestDefectB_RemoveDoesNotBlockTick();
    TestDefectC_ThrowingGetDataDoesNotCrashOrClobberCache();
    TestDefectD_BoundedSubscribersAcrossBindUnbindCycles();
    TestDefectE_DedupAndStateGuard();

    if (g_failures == 0) {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
