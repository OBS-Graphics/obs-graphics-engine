// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "uuid.h"

#include <string>
#include <unordered_map>
#include <vector>

using Record = std::unordered_map<std::string, std::string>;

// What a data source knows of a Title: its identity, and nothing else. A
// source never sees a Title object — Scene (scene.h) publishes this directory
// into every source once per tick (IDataSource::SetTitleDirectory), and hands
// one back on every NotifyTriggerIn/NotifyTriggerOut. `id` is the Title's
// uuid (unique within a Scene, which is what a script targets with
// trigger_out); `name` is the human-readable name, which may be shared by
// several Titles — hence the script-side lookup returning a list.
struct TitleRef {
    std::string id;
    std::string name;

    bool operator==(const TitleRef& o) const { return id == o.id && name == o.name; }
    bool operator!=(const TitleRef& o) const { return !(*this == o); }
};

// A source of records that `DataPool` (data-pool.h) owns, polls, and caches.
// A source knows nothing about which Title(s) consume it, nor how many there
// are — Titles pull the pool's cache themselves by source id, which is what
// lets several Titles share one instance (e.g. a weather feed backing both a
// corner ticker and a full-screen graphic) with no per-Title state here.
struct IDataSource {
    virtual ~IDataSource() = default;

    // Stable identity, generated at construction. This is what a Title stores
    // in `Title::dataSourceId` and what DataPool keys its registry by.
    // SetId() exists so a host can restore an id it persisted with its own
    // project/collection settings; changing it while the source is registered
    // with a DataPool does NOT re-key the pool (Remove/Add for that).
    const std::string& GetId() const { return m_id; }
    void SetId(std::string id) { m_id = std::move(id); }

    virtual std::vector<Record> GetData() const = 0;
    virtual std::string GetFilePath() const = 0;

    // Like GetData(), but for sources that fetch asynchronously (e.g. a
    // worker-thread-backed ScriptDataSource), waits for a fresh fetch to
    // complete (bounded) instead of returning whatever's currently cached.
    // Used by DataPool::DataBlocking(), which Title::TriggerIn calls when
    // showing a Hidden title — where stale/blank data on first appearance
    // would be wrong. Synchronous sources (file-backed) are already always
    // fresh, so the default just delegates to GetData().
    virtual std::vector<Record> GetDataBlocking() const { return GetData(); }

    // Per-tick housekeeping hook, called by DataPool::Tick on every source
    // every frame. Default: nothing to do. Must not block.
    virtual void PumpEvents() const {}

    // Drains script-originated trigger_out(...) requests accumulated since the
    // last drain (DataPool::Tick collects these once per source per tick and
    // Scene::Tick applies them). Each returned string is a *Title uuid*; the
    // empty-string sentinel means "every title reading this source" (Lua's
    // bare trigger_out()). Default: nothing to drain — only ScriptDataSource
    // produces these.
    virtual std::vector<std::string> DrainOutTriggerRequests() { return {}; }

    // Called once per showing/hiding of a Title that reads this source, from
    // any origin (a host call, the `duration` timeout, or this same source's
    // own trigger_out()). Default: no-op — only sources that care about a
    // title's show/hide (e.g. ScriptDataSource, relaying into
    // _on_trigger_in(title, ...)/_on_trigger_out(title)) override these.
    virtual void NotifyTriggerIn(const TitleRef& title, size_t recordIndex, double duration) {}
    virtual void NotifyTriggerOut(const TitleRef& title) {}

    // Every Title in the Scene this source belongs to, pushed down by
    // Scene::Tick whenever the set changes. This is what backs the Lua
    // `scene.find_titles(name)`/`scene.titles()` lookups — a source that
    // exposes no script surface has nothing to do with it. Default: no-op.
    virtual void SetTitleDirectory(std::vector<TitleRef> titles) {}

private:
    std::string m_id{uuid::GenerateV4()};
};

struct JsonFileDataSource : public IDataSource {
    std::string filePath;

    JsonFileDataSource(const std::string& path) : filePath(path) {}
    std::vector<Record> GetData() const override;
    std::string GetFilePath() const override
    {
        return filePath;
    }
};

struct CsvFileDataSource : public IDataSource {
    std::string filePath;

    CsvFileDataSource(const std::string& path) : filePath(path) {}
    std::vector<Record> GetData() const override;
    std::string GetFilePath() const override
    {
        return filePath;
    }
};
