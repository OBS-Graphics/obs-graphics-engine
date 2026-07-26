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
//
// A later review pass of the same DataPool found the defects above fixed but
// the following behaviors uncovered by any test:
//
//  - Gap 1: Add() over an existing key transparently re-binds every Title
//    already bound to that key onto the NEW source -- both the data path
//    (cache refresh) and the trigger-routing path (NotifyTriggerOut), with
//    no re-Bind() call from the host.
//  - Gap 2: each of the three teardown paths (Unbind(), Remove(), Clear())
//    neutralizes a Title's installed subscriber so a trigger fired on it
//    afterward cannot reach the (possibly already-destroyed) source.
//  - Gap 3: Bind()ing an already-bound Title onto a different key replaces
//    the binding rather than duplicating it -- BoundTitleNames() on the old
//    key stops listing it, and only the new key's source ever sees it again.
//  - Gap 4: the cache refresh cadence is gated on visibility -- it does NOT
//    advance while every bound Title is Hidden, and resumes once one becomes
//    Visible again.
//  - Gap 5: two Titles bound to the same key both read from that key's one
//    shared cache/fetch, and trigger_out(bindName) aimed at one of them
//    leaves the other untouched.
//  - Gap 6: DataBlocking() on an unknown key returns {} immediately (no
//    blocking, no throwing); on a known key it performs a fresh fetch.
//  - Gap 7: BoundTitleNames(key) always reflects exactly the currently-bound
//    names for that key, including after Unbind()/Remove().

#include "data-pool.h"
#include "title.h"
#include "script_test_util.h"

#include <algorithm>
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

// Gaps 1-5: a source with a distinguishable, settable `label` (so a test can
// tell "the old source" and "the new source" apart after Add() replaces one
// under the same key) plus counters for GetData()/NotifyTriggerOut() calls
// (so a test can tell exactly which source instance a Title's routed trigger
// actually reached).
struct LabeledSource : IDataSource {
    std::string label;
    mutable int calls = 0;
    int outCount = 0;
    std::vector<Record> GetData() const override
    {
        ++calls;
        return { Record{{"value", label}} };
    }
    std::string GetFilePath() const override { return ""; }
    void NotifyTriggerOut(const std::string&) override { ++outCount; }
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

// ── Gap 1: Add() over an existing key transparently re-binds ───────────────

void TestGap1_AddRebindsExistingBindingsTransparently()
{
    DataPool pool;
    auto first = std::make_unique<LabeledSource>();
    first->label = "first";
    pool.Add("k1", std::move(first));

    Title title;
    pool.Bind(&title, "k1", "bindA");
    title.TriggerIn();
    title.Tick(0.01f);
    Check(title.state == TitleState::Visible, "Gap1 setup: title reached Visible");

    for (int i = 0; i < 30; ++i) {
        title.Tick(0.01f);
        pool.Tick(0.01f);
    }
    auto cache1 = pool.Data("k1");
    Check(!cache1.empty() && cache1[0].at("value") == "first",
          "Gap1 setup: cache reflects the first source before any rebind");

    // Replace the source registered under the same key -- no re-Bind() call
    // on `title` from here on.
    auto second = std::make_unique<LabeledSource>();
    second->label = "second";
    LabeledSource* secondRaw = second.get();
    pool.Add("k1", std::move(second));

    Check(pool.BoundTitleNames("k1").size() == 1,
          "Gap1: title is still counted as bound to k1 after Add() swapped the source");

    for (int i = 0; i < 30; ++i) {
        title.Tick(0.01f);
        pool.Tick(0.01f);
    }
    auto cache2 = pool.Data("k1");
    Check(!cache2.empty() && cache2[0].at("value") == "second",
          "Gap1: cache now reflects the NEW source after Add() replaced it under the same key, no re-Bind() needed");

    // Trigger-routing must also have followed the swap: TriggerOut() on the
    // still-bound title must reach the NEW source (the old one was destroyed
    // when Add() replaced it).
    title.TriggerOut();
    Check(secondRaw->outCount == 1,
          "Gap1: TriggerOut() after Add() reaches the new source, not the destroyed old one");
}

// ── Gap 2: teardown neutralizes installed subscribers ───────────────────────

void TestGap2_TeardownNeutralizesSubscribers()
{
    // Path A: Unbind() -- the source stays registered (not destroyed), so
    // this is checked directly: the subscriber must go quiet but the source
    // itself remains safe to read.
    {
        DataPool pool;
        auto srcOwned = std::make_unique<LabeledSource>();
        srcOwned->label = "u";
        LabeledSource* raw = srcOwned.get();
        pool.Add("u", std::move(srcOwned));

        Title title;
        pool.Bind(&title, "u", "b");
        title.TriggerOut();
        Check(raw->outCount == 1, "Gap2a setup: source receives TriggerOut() while bound");

        pool.Unbind(&title);
        title.TriggerOut();
        Check(raw->outCount == 1,
              "Gap2a: after Unbind(), a direct TriggerOut() no longer reaches the (still-registered) source");
    }

    // Path B: Remove() -- the source is destroyed once m_mutex is released.
    // `raw` becomes dangling right after Remove() returns and MUST NOT be
    // dereferenced again -- the point of this test is that the correct
    // implementation never lets a later TriggerOut() try to. If a future
    // regression reordered Remove() to destroy the source before
    // neutralizing the Title's subscriber, the TriggerOut() call below would
    // be a use-after-free (most likely a crash) instead of a clean FAIL
    // line -- the same failure mode already documented for Defect B/C.
    {
        DataPool pool;
        auto srcOwned = std::make_unique<LabeledSource>();
        srcOwned->label = "r";
        LabeledSource* raw = srcOwned.get();
        pool.Add("r", std::move(srcOwned));

        Title title;
        pool.Bind(&title, "r", "b");
        title.TriggerOut();
        Check(raw->outCount == 1, "Gap2b setup: source receives TriggerOut() while bound");

        pool.Remove("r");  // `raw` is dangling from here on -- never touched again.
        title.TriggerOut();
        Check(pool.BoundTitleNames("r").empty(), "Gap2b: no bindings remain for the removed key");
        Check(!pool.Has("r"), "Gap2b: key is gone after Remove()");
    }

    // Path C: Clear() -- same dangling-pointer caveat as Remove() above,
    // just for every source/binding in the pool at once.
    {
        DataPool pool;
        auto srcOwned = std::make_unique<LabeledSource>();
        srcOwned->label = "c";
        pool.Add("c", std::move(srcOwned));

        Title title;
        pool.Bind(&title, "c", "b");
        title.TriggerOut();

        pool.Clear();  // the source is dangling from here on -- never touched again.
        title.TriggerOut();
        Check(pool.BoundTitleNames("c").empty(), "Gap2c: no bindings remain for any key after Clear()");
        Check(!pool.Has("c"), "Gap2c: key is gone after Clear()");
        Check(pool.Keys().empty(), "Gap2c: no sources remain after Clear()");
    }
}

// ── Gap 3: Bind() on an already-bound Title replaces, not duplicates ───────

void TestGap3_RebindReplacesNotDuplicates()
{
    DataPool pool;
    auto srcA = std::make_unique<LabeledSource>();
    srcA->label = "A";
    LabeledSource* rawA = srcA.get();
    pool.Add("A", std::move(srcA));

    auto srcB = std::make_unique<LabeledSource>();
    srcB->label = "B";
    LabeledSource* rawB = srcB.get();
    pool.Add("B", std::move(srcB));

    Title title;
    pool.Bind(&title, "A", "bindX");
    Check(pool.BoundTitleNames("A") == std::vector<std::string>{"bindX"}, "Gap3 setup: title bound to A");

    pool.Bind(&title, "B", "bindX");  // re-bind the SAME title onto a different key
    Check(pool.BoundTitleNames("A").empty(),
          "Gap3: BoundTitleNames(A) no longer lists the title after it rebinds to B");
    Check(pool.BoundTitleNames("B") == std::vector<std::string>{"bindX"},
          "Gap3: BoundTitleNames(B) lists the title, not duplicated onto both keys");

    title.TriggerIn();
    title.Tick(0.01f);
    for (int i = 0; i < 30; ++i) {
        title.Tick(0.01f);
        pool.Tick(0.01f);
    }
    auto cacheB = pool.Data("B");
    Check(!cacheB.empty() && cacheB[0].at("value") == "B", "Gap3: B's cache refreshed for the now-Visible bound title");

    // Trigger-routing: only B's source must ever see this title again.
    title.TriggerOut();
    Check(rawB->outCount == 1, "Gap3: TriggerOut() reaches B's source");
    Check(rawA->outCount == 0, "Gap3: TriggerOut() does not reach A's source after rebinding away from it");
}

// ── Gap 4: cadence is gated on visibility ───────────────────────────────────

void TestGap4_CadenceGatedByVisibility()
{
    DataPool pool;
    auto srcOwned = std::make_unique<FlakyDataSource>();
    FlakyDataSource* src = srcOwned.get();
    pool.Add("g4", std::move(srcOwned));  // Add() primes with one synchronous fetch

    Title title;  // starts Hidden
    pool.Bind(&title, "g4", "n");

    int callsAfterAdd = src->calls;
    for (int i = 0; i < 90; ++i) pool.Tick(0.01f);  // ~0.9s, well past one cadence, title stays Hidden throughout
    Check(src->calls == callsAfterAdd, "Gap4: cache does not refresh while every bound title is Hidden");

    title.TriggerIn();
    title.Tick(0.01f);
    Check(title.state == TitleState::Visible, "Gap4 setup: title became Visible");

    for (int i = 0; i < 30; ++i) {
        title.Tick(0.01f);
        pool.Tick(0.01f);
    }
    Check(src->calls > callsAfterAdd, "Gap4: cache resumes refreshing once a bound title becomes Visible again");

    title.TriggerOut();
    float t = 0.0f;
    while (title.state == TitleState::AnimatingOut && t < 5.0f) {
        title.Tick(0.02f);
        t += 0.02f;
    }
    Check(title.state == TitleState::Hidden, "Gap4 setup: title reached Hidden again");

    int callsAtHidden = src->calls;
    for (int i = 0; i < 90; ++i) pool.Tick(0.01f);
    Check(src->calls == callsAtHidden, "Gap4: cache stops refreshing again once the title returns to Hidden");
}

// ── Gap 5: two Titles sharing one key ───────────────────────────────────────

void TestGap5_TwoTitlesShareOneKey()
{
    // Part 1: both bound titles read from the same single fetch -- the
    // source is polled once per cadence tick regardless of how many titles
    // are bound to it.
    DataPool pool;
    auto srcOwned = std::make_unique<LabeledSource>();
    srcOwned->label = "shared";
    LabeledSource* src = srcOwned.get();
    pool.Add("shared", std::move(srcOwned));

    Title titleA, titleB;
    pool.Bind(&titleA, "shared", "a");
    pool.Bind(&titleB, "shared", "b");

    {
        auto names = pool.BoundTitleNames("shared");
        std::sort(names.begin(), names.end());
        Check(names == std::vector<std::string>({"a", "b"}), "Gap5: both bind names are listed under the shared key");
    }

    titleA.TriggerIn();
    titleA.Tick(0.01f);
    titleB.TriggerIn();
    titleB.Tick(0.01f);
    Check(titleA.state == TitleState::Visible && titleB.state == TitleState::Visible,
          "Gap5 setup: both titles reached Visible");

    int callsBefore = src->calls;
    for (int i = 0; i < 30; ++i) {
        titleA.Tick(0.01f);
        titleB.Tick(0.01f);
        pool.Tick(0.01f);
    }
    Check(src->calls == callsBefore + 1,
          "Gap5: crossing one cadence boundary with 2 bound titles still fetches the source exactly once (shared, not duplicated)");
    auto cache = pool.Data("shared");
    Check(!cache.empty() && cache[0].at("value") == "shared",
          "Gap5: the single shared cache both bindings would read from holds the fetched records");

    // Part 2: trigger_out(bindNameOfA) affects only A.
    auto drainOwned = std::make_unique<ManualDrainSource>();
    ManualDrainSource* drain = drainOwned.get();
    pool.Add("shared2", std::move(drainOwned));

    Title titleC, titleD;
    pool.Bind(&titleC, "shared2", "c");
    pool.Bind(&titleD, "shared2", "d");
    titleC.TriggerIn();
    titleC.Tick(0.01f);
    titleD.TriggerIn();
    titleD.Tick(0.01f);
    Check(titleC.state == TitleState::Visible && titleD.state == TitleState::Visible,
          "Gap5 setup: both titles (for the trigger_out test) reached Visible");

    drain->pending = {"c"};
    pool.Tick(0.01f);
    Check(titleC.state == TitleState::AnimatingOut, "Gap5: trigger_out(\"c\") moves titleC out");
    Check(titleD.state == TitleState::Visible,
          "Gap5: trigger_out(\"c\") leaves titleD (bound to the same key, different bind name) untouched");
}

// ── Gap 6: DataBlocking() on unknown vs. known keys ─────────────────────────

void TestGap6_DataBlockingUnknownAndKnownKeys()
{
    DataPool pool;

    auto start = std::chrono::steady_clock::now();
    auto result = pool.DataBlocking("nope");
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
    Check(result.empty(), "Gap6: DataBlocking() on an unknown key returns empty");
    Check(elapsedMs < 50, "Gap6: DataBlocking() on an unknown key returns immediately without blocking");

    auto srcOwned = std::make_unique<FlakyDataSource>();
    FlakyDataSource* src = srcOwned.get();
    pool.Add("known", std::move(srcOwned));  // Add() already performs one priming fetch
    int callsBefore = src->calls;

    auto fresh = pool.DataBlocking("known");
    Check(src->calls == callsBefore + 1, "Gap6: DataBlocking() on a known key performs a fresh fetch");
    Check(!fresh.empty() && fresh[0].at("value") == "good", "Gap6: DataBlocking() on a known key returns fresh data");
    auto cached = pool.Data("known");
    Check(!cached.empty() && cached[0].at("value") == "good",
          "Gap6: DataBlocking() also refreshes the pool's cache for a subsequent Data() call");
}

// ── Gap 7: BoundTitleNames() tracks bindings exactly ────────────────────────

void TestGap7_BoundTitleNamesTracksBindings()
{
    DataPool pool;
    pool.Add("k7", std::make_unique<FlakyDataSource>());

    Check(pool.BoundTitleNames("never-registered").empty(),
          "Gap7: BoundTitleNames() on a never-registered key returns empty");

    Title t1, t2, t3;
    pool.Bind(&t1, "k7", "one");
    pool.Bind(&t2, "k7", "two");
    pool.Bind(&t3, "k7", "three");

    auto names = pool.BoundTitleNames("k7");
    std::sort(names.begin(), names.end());
    Check(names == std::vector<std::string>({"one", "three", "two"}),
          "Gap7: BoundTitleNames lists exactly the three currently-bound names");

    pool.Unbind(&t2);
    names = pool.BoundTitleNames("k7");
    std::sort(names.begin(), names.end());
    Check(names == std::vector<std::string>({"one", "three"}), "Gap7: BoundTitleNames drops a name after Unbind()");

    pool.Remove("k7");
    Check(pool.BoundTitleNames("k7").empty(), "Gap7: BoundTitleNames is empty for a removed key");
}

}  // namespace

int main()
{
    TestDefectA_AddPrimesCache();
    TestDefectB_RemoveDoesNotBlockTick();
    TestDefectC_ThrowingGetDataDoesNotCrashOrClobberCache();
    TestDefectD_BoundedSubscribersAcrossBindUnbindCycles();
    TestDefectE_DedupAndStateGuard();

    TestGap1_AddRebindsExistingBindingsTransparently();
    TestGap2_TeardownNeutralizesSubscribers();
    TestGap3_RebindReplacesNotDuplicates();
    TestGap4_CadenceGatedByVisibility();
    TestGap5_TwoTitlesShareOneKey();
    TestGap6_DataBlockingUnknownAndKnownKeys();
    TestGap7_BoundTitleNamesTracksBindings();

    if (g_failures == 0) {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
