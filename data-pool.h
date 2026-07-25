// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "data-source.h"
#include "title.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Owns every IDataSource a host registers, polls each once per tick, and
// pushes freshly-fetched records into every Title bound to it.
//
// This exists so several Titles can share one data source. Before DataPool,
// a Title owned an IDataSource* directly and IDataSource::SetOwner tracked a
// single Title back-pointer; polling two Titles off the same instance made
// them fight over that pointer (and re-register duplicate onTriggerIn/
// onTriggerOut callbacks on every single poll — see the ScriptDataSource
// history for how fast that grows unbounded). DataPool removes the
// single-owner assumption entirely: a source knows nothing about any Title,
// and any number of Titles can be bound to the same source key.
//
// ── Locking discipline ──
// Lock order is strictly DataPool's own mutex -> a bound Title's lock (when
// the host supplies one via Bind's `titleLock`). NEVER the reverse — a
// caller that already holds a Title's lock must not call into DataPool
// expecting DataPool to (re-)acquire that same lock, and DataPool never
// calls back into arbitrary host code while holding a lock it doesn't own.
// DataBlocking() is the one path that must not nest the two: it copies out
// the raw IDataSource* while holding the pool mutex, releases the mutex, and
// only then calls the (potentially slow — seconds, for a network-backed
// ScriptDataSource) GetDataBlocking(), so a slow fetch never blocks Tick(),
// Add()/Remove(), or any other binding's Data() on another thread.
class DataPool {
public:
    DataPool() = default;
    ~DataPool() = default;
    DataPool(const DataPool&) = delete;
    DataPool& operator=(const DataPool&) = delete;

    // ── registry (host/UI thread) ──

    // Registers `source` under `key`. If `key` already names a source, the
    // old one is replaced and every Title currently bound to `key` is
    // transparently re-bound to the new source (same bindName, same
    // titleLock) — the host does not need to re-Bind each Title itself.
    void Add(const std::string& key, std::unique_ptr<IDataSource> source);

    // Removes the source registered under `key`, first unbinding every Title
    // bound to it (see Unbind). No-op if `key` is unknown.
    void Remove(const std::string& key);

    // Removes every source and every binding.
    void Clear();

    bool Has(const std::string& key) const;
    std::vector<std::string> Keys() const;

    // Host-UI-only escape hatch, e.g. to dynamic_cast to ScriptDataSource
    // and read GetLoadError(). Never call anything mutating/blocking on the
    // returned pointer from a thread other than the one DataPool itself uses
    // internally (GetData()/GetDataBlocking() are the exception — both are
    // documented safe to call from any thread on their own source type).
    IDataSource* Get(const std::string& key) const;

    // ── bindings ──

    // Binds `title` to the source registered under `key` so DataPool::Tick
    // starts pushing that source's refreshed records into `title` (while
    // `title->state == TitleState::Visible`) and starts relaying `title`'s
    // onTriggerIn/onTriggerOut firings into the source (see
    // IDataSource::NotifyTriggerIn/NotifyTriggerOut). `bindName` is the
    // identity a Lua script sees and can target via trigger_out(bindName) /
    // _on_trigger_in(bindName, ...) / _on_trigger_out(bindName) — deliberately
    // NOT Title::id, which comes straight from the loaded .ogt file (see
    // title.cpp's Load()) and can collide (two open instances of the same
    // file) or be empty, neither of which makes a usable script-facing
    // handle. `titleLock`, if given, is locked by the pool around every
    // mutation of *title (appending the trigger subscribers here, and later
    // TriggerOut()/UpdateData() calls from Tick()) — the host passes the
    // same mutex it uses to guard that Title elsewhere. A Title can only be
    // bound to one key at a time; binding an already-bound Title first
    // unbinds its previous binding. No-op if `key` is unknown.
    void Bind(Title* title, const std::string& key, std::string bindName, std::mutex* titleLock = nullptr);

    // Detaches `title` from whatever it's bound to, if anything. Safe to
    // call even if `title` was never bound. Does not touch the Title's
    // onTriggerIn/onTriggerOut vectors (they have no removal API — see
    // below); the previously-installed subscriber becomes a permanent no-op
    // instead, guarded by a pool-owned flag.
    void Unbind(Title* title);

    // The bindName of every Title currently bound to `key`, for a host UI
    // that wants to show e.g. "these titles use this source."
    std::vector<std::string> BoundTitleNames(const std::string& key) const;

    // ── data ──

    // Last cached records for `key` (empty if unknown or never fetched).
    // Never blocks, never touches the source itself — safe to call from any
    // thread, including from inside Tick() by re-entrant callers... though
    // in practice this is normally called from whichever thread wants a
    // one-off read (e.g. a host UI preview), not from Tick() itself.
    std::vector<Record> Data(const std::string& key) const;

    // Forces a fresh fetch from the source registered under `key` and
    // returns it (empty if `key` is unknown). Does NOT hold the pool mutex
    // while waiting — see the locking-discipline note above — so a slow
    // fetch never blocks Tick()/Add()/Remove()/Bind() on another thread. The
    // fetched result also refreshes the pool's cache for `key` (so a
    // subsequent Data(key) sees it too), but this is best-effort: if `key`
    // was Removed while the fetch was in flight, the result is simply
    // returned without being cached anywhere.
    std::vector<Record> DataBlocking(const std::string& key);

    // ── per-frame (host render thread) ──

    // Advances every registered source and binding by one frame, in order:
    //  (a) PumpEvents() on every source (per-tick housekeeping hook), then
    //      DrainOutTriggerRequests() to collect any script-originated
    //      trigger_out(bindName) requests, mapping each requested bind name
    //      (or the empty-string "all bound titles" sentinel) back to the
    //      Title(s) bound to that source and calling title->TriggerOut() on
    //      each match;
    //  (b) refresh each source's cache on a fixed 0.25s cadence — but only
    //      while at least one Title currently bound to it is
    //      TitleState::Visible, mirroring the old per-Title gating
    //      (Title::Tick only ever polled while state == Visible) now
    //      evaluated across every binding sharing the source instead of a
    //      single owning Title. A source with zero Visible bindings simply
    //      doesn't advance its cadence timer, so it resumes exactly where
    //      it left off once something becomes Visible again — same
    //      prev-slot/curr-slot integer-division technique Title::Tick used
    //      to run per-title before this class existed, just tracked
    //      per-source now instead of per-title;
    //  (c) for every binding whose Title is TitleState::Visible and whose
    //      source refreshed this tick, call title->UpdateData(records,
    //      /*instant=*/false).
    void Tick(float dt);

private:
    struct Entry {
        std::unique_ptr<IDataSource> source;
        std::vector<Record> cache;
        double updateTimer{0.0};
        double prevUpdateTimer{0.0};
        bool refreshedThisTick{false};
    };

    struct Binding {
        Title* title{nullptr};
        std::string key;
        std::string bindName;
        std::mutex* titleLock{nullptr};
        // Neutralizes the onTriggerIn/onTriggerOut subscribers installed on
        // `title` the moment this binding is torn down (Unbind/Remove/Add's
        // re-bind/Clear), so a callback firing mid-teardown or after the
        // underlying source is gone is a guaranteed no-op rather than a
        // use-after-free. Same discipline ScriptDataSource itself uses
        // (m_alive) for its own teardown — see IDataSource::AliveToken.
        std::shared_ptr<std::atomic<bool>> alive;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_sources;
    std::vector<Binding> m_bindings;

    // Assumes m_mutex is already held by the caller.
    void BindLocked(Title* title, const std::string& key, std::string bindName, std::mutex* titleLock);
    void UnbindLocked(Title* title);
    void TriggerOutLocked(Binding& binding);
    bool AnyBoundTitleVisibleLocked(const std::string& key) const;
};
