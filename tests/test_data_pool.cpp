// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test (no SDL, no rendering, no Lua) for DataPool
// (data-pool.h/cpp) on its own — the pool knows nothing about Title, so
// nothing here constructs one (see test_scene.cpp for the consumer side).
// Fake IDataSource implementations stand in for a slow/throwing/scripted
// source, so none of this needs ScriptDataSource or a real Lua script.
//
//  - Registration is keyed by the source's own generated id, and Add() primes
//    the cache with one fetch so Data(id) is meaningful immediately.
//  - The changed flag: a cache's version moves only when a refresh produced
//    records that actually differ, and DataIfChanged reflects that.
//  - Add()/Remove()/Clear() must not destroy a source while holding the pool
//    mutex — a slow ~IDataSource must never stall a concurrent Tick().
//  - A throwing GetData() must not escape Tick()/DataBlocking() (which would
//    std::terminate on the host's render thread); the previous cache is kept
//    on failure instead of being clobbered with {}.
//  - The relay/pass-through surface Title and Scene use: NotifyTriggerIn/Out,
//    PublishTitleDirectory, DrainOutTriggerRequests.

#include "data-pool.h"
#include "script_test_util.h"
#include "uuid.h"

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

// Long enough that a full 0.25s refresh slot is crossed in one Tick().
constexpr float kRefreshDt = 0.30f;

// ── Fakes ────────────────────────────────────────────────────────────────

// A destructor slow enough that, if it ever ran under DataPool's own mutex, a
// concurrent Tick() call would visibly stall.
struct SlowDtorSource : IDataSource {
    std::chrono::milliseconds sleepFor;
    explicit SlowDtorSource(std::chrono::milliseconds s) : sleepFor(s) {}
    ~SlowDtorSource() override { std::this_thread::sleep_for(sleepFor); }
    std::vector<Record> GetData() const override { return {}; }
    std::string GetFilePath() const override { return ""; }
};

// Returns whatever `value` currently is, counting calls, and can be told to
// throw instead.
struct FlakyDataSource : IDataSource {
    mutable int calls = 0;
    bool shouldThrow = false;
    std::string value = "good";
    std::vector<Record> GetData() const override
    {
        ++calls;
        if (shouldThrow)
            throw std::runtime_error("simulated read failure");
        return { Record{{"value", value}} };
    }
    std::string GetFilePath() const override { return ""; }
};

// Records everything the pool relays into a source.
struct RelaySpySource : IDataSource {
    int inCount = 0, outCount = 0;
    TitleRef lastIn, lastOut;
    size_t lastRecordIndex = 0;
    double lastDuration = 0.0;
    std::vector<TitleRef> directory;
    std::vector<std::string> pending;  // what the next drain will hand back

    std::vector<Record> GetData() const override { return {}; }
    std::string GetFilePath() const override { return ""; }

    void NotifyTriggerIn(const TitleRef& t, size_t idx, double dur) override
    {
        ++inCount;
        lastIn = t;
        lastRecordIndex = idx;
        lastDuration = dur;
    }
    void NotifyTriggerOut(const TitleRef& t) override { ++outCount; lastOut = t; }
    void SetTitleDirectory(std::vector<TitleRef> titles) override { directory = std::move(titles); }
    std::vector<std::string> DrainOutTriggerRequests() override
    {
        auto out = std::move(pending);
        pending.clear();
        return out;
    }
};

// ── Registration & priming ───────────────────────────────────────────────

void TestAddKeysBySourceIdAndPrimes()
{
    DataPool pool;
    auto srcOwned = std::make_unique<FlakyDataSource>();
    FlakyDataSource* src = srcOwned.get();
    const std::string generated = src->GetId();

    Check(uuid::IsV4(generated), "Add: a source generates a v4 uuid at construction");

    const std::string id = pool.Add(std::move(srcOwned));
    Check(id == generated, "Add: registers under the source's own id and returns it");
    Check(pool.Has(id), "Add: Has(id) is true afterwards");
    Check(pool.Get(id) == src, "Add: Get(id) hands back the same instance");

    Check(src->calls == 1, "Add: primed the cache with exactly one fetch");
    auto cached = pool.Data(id);
    Check(!cached.empty() && cached[0].at("value") == "good",
          "Add: Data(id) sees real records immediately, with no Tick() yet");
    Check(pool.DataVersion(id) == 1, "Add: a primed cache is at version 1");
}

void TestRemoveAndClear()
{
    DataPool pool;
    auto a = std::make_unique<FlakyDataSource>();
    auto b = std::make_unique<FlakyDataSource>();
    const std::string idA = pool.Add(std::move(a));
    const std::string idB = pool.Add(std::move(b));
    Check(pool.Ids().size() == 2, "Remove: two distinct sources coexist (ids don't collide)");

    pool.Remove(idA);
    Check(!pool.Has(idA) && pool.Has(idB), "Remove: drops only the named source");
    Check(pool.Data(idA).empty(), "Remove: a removed source's cache reads empty");
    Check(pool.DataVersion(idA) == 0, "Remove: a removed source's version reads 0");
    Check(pool.DataBlocking(idA).empty(), "DataBlocking: unknown id returns {} without blocking");

    pool.Remove(idA);  // must be a no-op, not a crash
    pool.Clear();
    Check(pool.Ids().empty(), "Clear: removes everything");
}

// ── The changed flag ─────────────────────────────────────────────────────

void TestVersionOnlyMovesOnRealChange()
{
    DataPool pool;
    auto srcOwned = std::make_unique<FlakyDataSource>();
    FlakyDataSource* src = srcOwned.get();
    const std::string id = pool.Add(std::move(srcOwned));

    const uint64_t primed = pool.DataVersion(id);

    pool.Tick(kRefreshDt);
    Check(src->calls == 2, "Version: Tick() refreshed once past the 0.25s slot");
    Check(pool.DataVersion(id) == primed,
          "Version: an identical refetch does NOT move the version");

    src->value = "changed";
    pool.Tick(kRefreshDt);
    Check(src->calls == 3, "Version: second refresh happened");
    Check(pool.DataVersion(id) == primed + 1, "Version: a differing refetch moves the version once");
    Check(pool.Data(id)[0].at("value") == "changed", "Version: the cache holds the new records");

    // Back to the original value: still a change relative to the cache.
    src->value = "good";
    pool.Tick(kRefreshDt);
    Check(pool.DataVersion(id) == primed + 2, "Version: changing back is still a change");
}

void TestDataIfChanged()
{
    DataPool pool;
    auto srcOwned = std::make_unique<FlakyDataSource>();
    FlakyDataSource* src = srcOwned.get();
    const std::string id = pool.Add(std::move(srcOwned));

    uint64_t seen = 0;
    std::vector<Record> out;
    Check(pool.DataIfChanged(id, seen, out), "DataIfChanged: first read (version 0) is a change");
    Check(seen == 1 && !out.empty() && out[0].at("value") == "good",
          "DataIfChanged: fills the records and advances the caller's version");

    out.clear();
    Check(!pool.DataIfChanged(id, seen, out), "DataIfChanged: unchanged cache reports no change");
    Check(out.empty(), "DataIfChanged: leaves the out-parameter untouched when unchanged");

    src->value = "fresh";
    pool.Tick(kRefreshDt);
    Check(pool.DataIfChanged(id, seen, out), "DataIfChanged: a real refresh reports a change");
    Check(out[0].at("value") == "fresh", "DataIfChanged: hands back the new records");

    uint64_t stale = 0;
    Check(!pool.DataIfChanged("no-such-source", stale, out),
          "DataIfChanged: unknown id reports no change");

    // A reader whose version is *ahead* (its source was removed and re-added,
    // restarting at 1) must still see a change — the compare is inequality,
    // not ordering.
    uint64_t ahead = 999;
    Check(pool.DataIfChanged(id, ahead, out), "DataIfChanged: a version ahead of the cache still reads as changed");
}

// ── Blocking fetch ───────────────────────────────────────────────────────

void TestDataBlocking()
{
    DataPool pool;
    auto srcOwned = std::make_unique<FlakyDataSource>();
    FlakyDataSource* src = srcOwned.get();
    const std::string id = pool.Add(std::move(srcOwned));

    src->value = "blocking";
    auto records = pool.DataBlocking(id);
    Check(src->calls == 2, "DataBlocking: performed a fresh fetch");
    Check(!records.empty() && records[0].at("value") == "blocking", "DataBlocking: returned the fresh records");
    Check(pool.Data(id)[0].at("value") == "blocking", "DataBlocking: also refreshed the pool cache");
    Check(pool.DataVersion(id) == 2, "DataBlocking: bumped the version, since the records differed");

    const uint64_t before = pool.DataVersion(id);
    pool.DataBlocking(id);
    Check(pool.DataVersion(id) == before, "DataBlocking: an identical refetch does not move the version");
}

// ── Failure handling ─────────────────────────────────────────────────────

void TestThrowingSourceIsContained()
{
    DataPool pool;
    auto srcOwned = std::make_unique<FlakyDataSource>();
    FlakyDataSource* src = srcOwned.get();
    const std::string id = pool.Add(std::move(srcOwned));  // primes with one good fetch

    src->shouldThrow = true;
    pool.Tick(kRefreshDt);  // must not propagate
    Check(true, "Throwing: Tick() survived a throwing GetData()");

    auto cached = pool.Data(id);
    Check(!cached.empty() && cached[0].at("value") == "good",
          "Throwing: the previous cache is kept, not clobbered with {}");

    auto blocked = pool.DataBlocking(id);
    Check(!blocked.empty() && blocked[0].at("value") == "good",
          "Throwing: DataBlocking() returns the last good cache instead of {}");

    src->shouldThrow = false;
    src->value = "recovered";
    pool.Tick(kRefreshDt);
    Check(pool.Data(id)[0].at("value") == "recovered", "Throwing: the cache recovers on the next good fetch");
}

void TestAddWithFailedPrimingFetch()
{
    DataPool pool;
    auto srcOwned = std::make_unique<FlakyDataSource>();
    srcOwned->shouldThrow = true;
    FlakyDataSource* src = srcOwned.get();
    const std::string id = pool.Add(std::move(srcOwned));

    Check(pool.Has(id), "FailedPrime: the source is still registered when priming threw");
    Check(pool.Data(id).empty(), "FailedPrime: its cache is empty");
    Check(pool.DataVersion(id) == 0, "FailedPrime: version 0 marks 'never successfully fetched'");

    src->shouldThrow = false;
    pool.Tick(kRefreshDt);
    Check(pool.DataVersion(id) == 1, "FailedPrime: the first successful fetch becomes version 1");
}

// ── Slow destruction must not stall Tick() ───────────────────────────────

void TestRemoveDoesNotBlockTick()
{
    DataPool pool;
    const std::string id = pool.Add(std::make_unique<SlowDtorSource>(std::chrono::milliseconds(400)));

    std::atomic<bool> removeDone{false};
    std::thread remover([&] {
        pool.Remove(id);
        removeDone.store(true, std::memory_order_release);
    });

    // While `remover` is presumably blocked inside ~SlowDtorSource, Tick() on
    // this thread must keep returning promptly — if Remove() destroyed the
    // source under m_mutex, one of these Tick() calls would stall for ~400ms
    // waiting on that same mutex.
    long worstCaseMs = 0;
    int iterations = 0;
    while (!removeDone.load(std::memory_order_acquire) && iterations < 20000) {
        auto tickStart = std::chrono::steady_clock::now();
        pool.Tick(0.01f);
        auto tickMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - tickStart).count();
        if (tickMs > worstCaseMs) worstCaseMs = tickMs;
        ++iterations;
    }
    remover.join();

    Check(iterations > 0, "SlowDtor: at least one Tick() ran concurrently with Remove()'s slow destructor");
    Check(worstCaseMs < 200, "SlowDtor: no single Tick() call stalled anywhere near the destructor's 400ms sleep");
}

void TestClearDoesNotBlockTick()
{
    DataPool pool;
    pool.Add(std::make_unique<SlowDtorSource>(std::chrono::milliseconds(400)));

    std::atomic<bool> clearDone{false};
    std::thread clearer([&] {
        pool.Clear();
        clearDone.store(true, std::memory_order_release);
    });

    long worstCaseMs = 0;
    while (!clearDone.load(std::memory_order_acquire)) {
        auto tickStart = std::chrono::steady_clock::now();
        pool.Tick(0.01f);
        auto tickMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - tickStart).count();
        if (tickMs > worstCaseMs) worstCaseMs = tickMs;
    }
    clearer.join();

    Check(worstCaseMs < 200, "SlowDtor: Clear() also destroys outside the mutex");
}

// ── Relays (what Title and Scene call) ───────────────────────────────────

void TestRelays()
{
    DataPool pool;
    auto spyOwned = std::make_unique<RelaySpySource>();
    RelaySpySource* spy = spyOwned.get();
    const std::string id = pool.Add(std::move(spyOwned));

    const TitleRef ref{"title-uuid", "Lower Third"};

    pool.NotifyTriggerIn(id, ref, 3, 1.5);
    Check(spy->inCount == 1 && spy->lastIn == ref, "Relay: NotifyTriggerIn reaches the source with the TitleRef");
    Check(spy->lastRecordIndex == 3 && spy->lastDuration == 1.5, "Relay: recordIndex/duration forwarded");

    pool.NotifyTriggerOut(id, ref);
    Check(spy->outCount == 1 && spy->lastOut == ref, "Relay: NotifyTriggerOut reaches the source");

    pool.NotifyTriggerIn("no-such-source", ref, 0, -1.0);
    pool.NotifyTriggerOut("no-such-source", ref);
    Check(spy->inCount == 1 && spy->outCount == 1, "Relay: an unknown source id is a silent no-op");

    std::vector<TitleRef> directory{ref, TitleRef{"other-uuid", "Ticker"}};
    pool.PublishTitleDirectory(directory);
    Check(spy->directory == directory, "Relay: PublishTitleDirectory reaches every source verbatim");

    Check(pool.DrainOutTriggerRequests().empty(), "Drain: nothing pending yields an empty result");

    spy->pending = {"title-uuid", ""};
    auto drained = pool.DrainOutTriggerRequests();
    Check(drained.size() == 1 && drained[0].first == id, "Drain: groups requests under their source id");
    Check(drained[0].second.size() == 2 && drained[0].second[0] == "title-uuid" && drained[0].second[1].empty(),
          "Drain: hands back every queued request, sentinel included");
    Check(pool.DrainOutTriggerRequests().empty(), "Drain: a second drain is empty (the source was drained)");
}

}  // namespace

int main()
{
    TestAddKeysBySourceIdAndPrimes();
    TestRemoveAndClear();
    TestVersionOnlyMovesOnRealChange();
    TestDataIfChanged();
    TestDataBlocking();
    TestThrowingSourceIsContained();
    TestAddWithFailedPrimingFetch();
    TestRemoveDoesNotBlockTick();
    TestClearDoesNotBlockTick();
    TestRelays();

    if (g_failures == 0) {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
