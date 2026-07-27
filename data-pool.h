// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "data-source.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Owns every IDataSource in a Scene, polls each on a fixed cadence, and caches
// what it got. That is the whole job.
//
// DataPool knows nothing about Title — it never includes title.h, never holds
// a Title*, and never calls into one. Consumers *pull*: a Title stores the id
// of the source it reads (Title::dataSourceId) and asks this cache for records
// in its own Tick, and Scene (scene.h) is what maps script-originated
// trigger_out(...) requests back onto Titles. This is deliberately the inverse
// of the earlier binding-based design, where the pool owned the Title <->
// source relationship and pushed into Titles from its own Tick.
//
// Each cache carries a `version` that only moves when the fetched records
// actually differ from what was cached, so a reader can tell "nothing changed"
// from "changed back to the same value" cheaply — see DataIfChanged(), which
// is what lets a Title poll every frame and still only animate on real change.
//
// ── Threading ──
// m_mutex protects this class's own registry and caches only, so a host UI
// thread can Add()/Remove() sources while the render thread Tick()s. DataPool
// never calls back into host code while holding it, with one exception it
// explicitly avoids: DataBlocking() copies out the raw IDataSource* under the
// lock, releases it, and only then runs the (potentially seconds-long, for a
// network-backed ScriptDataSource) GetDataBlocking() — so a slow fetch never
// stalls Tick(), Add(), or Remove() on another thread. Sources are likewise
// destroyed only after the lock is released (~ScriptDataSource joins a worker
// thread that can block for seconds finishing an in-flight fetch).
class DataPool {
public:
    DataPool() = default;
    ~DataPool() = default;
    DataPool(const DataPool&) = delete;
    DataPool& operator=(const DataPool&) = delete;

    // ── registry ──

    // Registers `source` under its own id (IDataSource::GetId) and returns
    // that id. If a source with the same id is already registered it is
    // replaced (and destroyed after m_mutex is released — see the threading
    // note above).
    //
    // Primes the cache with one synchronous GetData() before `source` is
    // registered anywhere, so a host that Adds a source and immediately reads
    // Data(id) to preview it doesn't see {} until the first cadence tick. That
    // priming call deliberately runs before m_mutex is ever taken, and a
    // throwing GetData() is caught and logged rather than propagated (same
    // guard as Tick()).
    std::string Add(std::unique_ptr<IDataSource> source);

    // Removes (and destroys, after m_mutex is released) the source with this
    // id. No-op if unknown. Titles still naming it in `dataSourceId` simply
    // read an empty cache from then on.
    void Remove(const std::string& sourceId);

    // Removes and destroys every source. Same after-the-lock destruction.
    void Clear();

    bool Has(const std::string& sourceId) const;
    std::vector<std::string> Ids() const;

    // Escape hatch for host UI, e.g. to dynamic_cast to ScriptDataSource and
    // read GetLoadError(). Don't call anything mutating on the returned
    // pointer from a thread other than the one driving Tick().
    IDataSource* Get(const std::string& sourceId) const;

    // ── data (any thread) ──

    // Last cached records (empty if the id is unknown or nothing was ever
    // fetched). Never blocks, never touches the source.
    std::vector<Record> Data(const std::string& sourceId) const;

    // Version of the current cache: 0 before anything was ever fetched, then
    // bumped only when a fetch produced records that differ from the cache.
    uint64_t DataVersion(const std::string& sourceId) const;

    // Combined read-if-changed: returns false (leaving `version`/`out`
    // untouched) when the id is unknown or the cache is still at `version`;
    // otherwise fills `out` with the cache, updates `version` to match, and
    // returns true. Compares versions for inequality, not ordering, so a
    // source that was removed and re-added (version restarting at 1) still
    // reads as changed.
    bool DataIfChanged(const std::string& sourceId, uint64_t& version, std::vector<Record>& out) const;

    // Forces a fresh fetch and returns it (empty if the id is unknown). Does
    // NOT hold m_mutex while waiting — see the threading note above. The
    // result also refreshes this cache (bumping `version` if it differs),
    // best-effort: if the source was removed or replaced while the fetch was
    // in flight, the records are returned without being cached. If the fetch
    // throws, the exception is caught and logged, the existing cache is left
    // untouched, and that cache is returned instead of {}.
    std::vector<Record> DataBlocking(const std::string& sourceId);

    // ── source-facing relays (host thread) ──
    // Everything that needs to talk *to* a source goes through these, so
    // nothing outside this class handles a raw IDataSource* lifetime.

    // Relays a Title's show/hide into the source it reads. No-op if the id is
    // unknown. Called by Title::TriggerIn/TriggerOut.
    void NotifyTriggerIn(const std::string& sourceId, const TitleRef& title, size_t recordIndex, double duration);
    void NotifyTriggerOut(const std::string& sourceId, const TitleRef& title);

    // Publishes the Scene's Title directory into the sources (see
    // IDataSource::SetTitleDirectory). Called by Scene::Tick every frame; the
    // change detection lives here rather than in the caller because only this
    // class knows which sources exist. A source is pushed to when *it* is
    // behind the current directory, not merely when the directory just moved:
    // a source registered after the last change — the Reload path, Add() over
    // an existing id — would otherwise sit on an empty directory forever,
    // silently breaking scene.find_titles/scene.titles for its script. An
    // already-current frame costs one vector compare plus one integer compare
    // per source.
    void PublishTitleDirectory(const std::vector<TitleRef>& titles);

    // Drains every source's pending script-originated trigger_out(...)
    // requests, as {sourceId, titleUuids}. Sources with nothing pending are
    // omitted, so the common result is an empty vector. Scene::Tick is what
    // resolves the uuids (and the empty-string "every title on this source"
    // sentinel) to Titles and applies TriggerOut().
    std::vector<std::pair<std::string, std::vector<std::string>>> DrainOutTriggerRequests();

    // ── per-frame ──

    // PumpEvents() every source, then refresh each cache on a fixed 0.25s
    // cadence, bumping that source's `version` only when the new records
    // differ from the cached ones. Unconditional: the pool has no idea which
    // Titles exist or whether any is on screen, by design.
    //
    // A throwing GetData() (a file source reading a missing/malformed file —
    // see JsonFileDataSource/CsvFileDataSource) is caught: Tick() runs on the
    // host's render thread, called from C code (libobs), so an escaping
    // exception would be std::terminate. On failure the previous cache is
    // kept (never clobbered with {}) and the failure — and the later recovery
    // — is logged once at the edge, not every 0.25s.
    void Tick(float dt);

private:
    struct Entry {
        std::unique_ptr<IDataSource> source;
        std::vector<Record> cache;
        uint64_t version{0};
        double updateTimer{0.0};
        double prevUpdateTimer{0.0};
        bool lastFetchFailed{false};
        // Which m_directoryVersion this source was last handed. 0 = none yet,
        // which is also what a freshly registered source starts at — that is
        // what makes a late Add() catch up.
        uint64_t publishedDirVersion{0};
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_sources;
    // Last directory handed to PublishTitleDirectory, and a counter bumped
    // only when it actually changed. Kept here (rather than in Scene) so a
    // source that arrives later can be brought up to date on its own.
    std::vector<TitleRef> m_titleDirectory;
    uint64_t m_directoryVersion{0};
};
