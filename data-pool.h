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
    //
    // Primes the pool's cache for `key` with one synchronous GetData() call
    // on `source` before it is registered anywhere (so this never touches
    // m_mutex and can't stall Tick() on another thread) — otherwise a host
    // that Adds a source and immediately calls Data(key) to preview it would
    // see {} until some Title bound to it goes Visible. A thrown exception
    // from that priming fetch is caught and logged, never propagated (see
    // the same guard in Tick()'s step (b)). If `key` already named a source,
    // the OLD source is destroyed only after `m_mutex` is released, exactly
    // like Remove()/Clear() below — never under the lock Tick() blocks on.
    void Add(const std::string& key, std::unique_ptr<IDataSource> source);

    // Removes the source registered under `key`, first unbinding every Title
    // bound to it (see Unbind). No-op if `key` is unknown. The removed
    // source is destroyed only after `m_mutex` is released — destroying a
    // ScriptDataSource joins its worker thread, which can block for seconds
    // finishing an in-flight fetch, and Tick() blocks on this same mutex
    // every frame on the host's render thread.
    void Remove(const std::string& key);

    // Removes every source and every binding. Every removed source is
    // destroyed only after `m_mutex` is released (see Remove()).
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
    // mutation of *title — installing the onTriggerIn/onTriggerOut
    // subscriber pair (only on the very first Bind() this Title ever sees;
    // see BindLocked/TriggerTarget), and later TriggerOut()/UpdateData()
    // calls from Tick() — the host passes the same mutex it uses to guard
    // that Title elsewhere. A Title can only be bound to one key at a time;
    // binding an already-bound Title first unbinds its previous binding.
    // No-op if `key` is unknown.
    void Bind(Title* title, const std::string& key, std::string bindName, std::mutex* titleLock = nullptr);

    // Detaches `title` from whatever it's bound to, if anything. Safe to
    // call even if `title` was never bound. Does NOT remove the
    // onTriggerIn/onTriggerOut subscriber pair installed on `title` the
    // first time it was ever Bind()'d (Title's onTriggerIn/onTriggerOut have
    // no removal API, and other subscribers — e.g. a host UI — share the
    // same vectors) — but that pair becomes an inert no-op: its shared
    // redirect target's `source` is cleared, so any firing between this call
    // and a future re-Bind() does nothing. A later Bind() of the same
    // `title` reuses that same subscriber pair (just repointing the target)
    // rather than installing a new one — this is what keeps
    // Bind()/Unbind() cycles (e.g. a host UI dropdown flipped back and
    // forth, or Add() reloading a source bound to several Titles) from
    // growing those vectors without bound.
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
    // returned without being cached anywhere. If the fetch throws, the
    // exception is caught and logged (never propagated — see the same guard
    // in Tick()'s step (b)), the pool's existing cache for `key` is left
    // untouched, and that existing cache is returned instead of {} — a
    // transient read error shouldn't hand the caller nothing when it has
    // something better on hand.
    std::vector<Record> DataBlocking(const std::string& key);

    // ── per-frame (host render thread) ──

    // Advances every registered source and binding by one frame, in order:
    //  (a) PumpEvents() on every source (per-tick housekeeping hook), then
    //      DrainOutTriggerRequests() to collect any script-originated
    //      trigger_out(bindName) requests, mapping each requested bind name
    //      (or the empty-string "all bound titles" sentinel) back to the
    //      Title(s) bound to that source and calling title->TriggerOut() on
    //      each match — deduplicated within this one drain, so a single pass
    //      that names the same bound Title more than once (e.g. a script
    //      calling both trigger_out("x") and the bare trigger_out() in the
    //      same _get_data run) fires that Title's TriggerOut() at most once,
    //      and skipped entirely for a Title that's already Hidden or
    //      AnimatingOut (so a bare trigger_out() aimed at a title that's
    //      already off-screen can't replay its out-animation);
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
    //      per-source now instead of per-title. The refresh fetch is
    //      guarded against a throwing GetData() (a file source throws on a
    //      missing/malformed file — see JsonFileDataSource/CsvFileDataSource
    //      in data-source.cpp): Tick() runs on the host's render thread,
    //      called from C code (libobs), so an escaping exception would be
    //      std::terminate. On failure the previous cache is left as-is
    //      (never clobbered with {}) and the failure (and later recovery) is
    //      logged once at the edge, not every 0.25s tick;
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
        // True once this source's GetData() has thrown, so the failure (and
        // later recovery) is logged only on the edge instead of every 0.25s
        // tick while it keeps failing.
        bool lastFetchFailed{false};
    };

    struct Binding {
        Title* title{nullptr};
        std::string key;
        std::string bindName;
        std::mutex* titleLock{nullptr};
    };

    // Persistent redirect cell for one Title's onTriggerIn/onTriggerOut
    // subscriber pair (see BindLocked). Deliberately NOT kept in a
    // DataPool-side map keyed by Title* — Bind()/Unbind() cycles need this
    // cell to survive across rebinds of the SAME Title (so its vectors don't
    // grow), but a Title* is just an address, and if a Title were ever
    // destroyed and a new, unrelated Title happened to be constructed at the
    // same address, a map keyed by that address would find this STALE cell
    // and (wrongly) conclude the new Title already has a subscriber
    // installed — silently leaving it never wired to any source. Instead,
    // the shared_ptr to a Title's TriggerTarget lives ONLY inside the thunk
    // object captured by the subscriber itself, reachable by scanning that
    // Title's own onTriggerOut for a TriggerOutThunk (see
    // FindOrInstallTriggerTargetLocked/NeutralizeTriggerTargetLocked) — a
    // genuinely fresh Title object's vectors start empty, so this can never
    // misfire regardless of address reuse.
    //
    // `mutex` guards this struct's own fields: the installed subscriber can
    // fire from any thread that calls title->TriggerIn()/TriggerOut()
    // directly (bypassing DataPool entirely), concurrently with a Bind()/
    // Unbind() on another thread mutating the same cell.
    struct TriggerTarget {
        std::mutex mutex;
        IDataSource* source{nullptr};
        std::string bindName;
        std::shared_ptr<const std::atomic<bool>> sourceAlive;
    };

    // Callable types installed onto Title::onTriggerIn/onTriggerOut.
    // Distinct types (rather than a generic lambda) so std::function::
    // target<T>() can find an already-installed one back out of a Title's
    // own vector — see TriggerTarget's comment above.
    struct TriggerInThunk {
        std::shared_ptr<TriggerTarget> target;
        void operator()(size_t recordIndex, double duration) const;
    };
    struct TriggerOutThunk {
        std::shared_ptr<TriggerTarget> target;
        void operator()() const;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_sources;
    std::vector<Binding> m_bindings;

    // Assumes m_mutex is already held by the caller.
    void BindLocked(Title* title, const std::string& key, std::string bindName, std::mutex* titleLock);
    void UnbindLocked(Title* title);
    void TriggerOutLocked(Binding& binding);
    bool AnyBoundTitleVisibleLocked(const std::string& key) const;

    // Finds the TriggerTarget already installed on `title` (by a previous
    // BindLocked call on this exact Title object), or installs a fresh pair
    // of subscribers plus a fresh TriggerTarget if none is found yet. Locks
    // `titleLock` (if given) around the whole find-or-install, since it
    // reads and potentially mutates *title.
    std::shared_ptr<TriggerTarget> FindOrInstallTriggerTargetLocked(Title* title, std::mutex* titleLock);

    // Clears `source`/`sourceAlive` on `title`'s installed TriggerTarget (if
    // it has one), making the subscriber pair an inert no-op until a future
    // Bind() repoints it. No-op if `title` has no TriggerTarget installed.
    // Locks `titleLock` (if given) around the operation.
    void NeutralizeTriggerTargetLocked(Title* title, std::mutex* titleLock);
};
