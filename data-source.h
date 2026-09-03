// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "uuid.h"

#include <mutex>
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

    // Operator-facing label. Empty means "no label of its own" — the host names
    // the source after GetFilePath() instead. Only a source with no file behind
    // it needs to override this.
    virtual std::string GetDisplayName() const { return {}; }

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

// A source with no file behind it at all — its records are typed in (or
// pasted) by an operator and held in memory. Everything else about it is
// identical to the file sources from DataPool's point of view: it still
// answers GetData() synchronously, still gets polled on the pool's cadence,
// and still costs one snapshot copy per fetch.
struct ManualDataSource : public IDataSource {
    // Metadata for the host's editor UI only — the engine treats every cell
    // as a string regardless of this tag. An "image" cell is just a
    // filesystem path, handed to ImageElement::ApplyContent like any other
    // image_path value; there is no engine-side image handling to look for.
    enum class ColumnType { Text, Image };

    struct Column {
        std::string name;                 // the element id the value is applied to
        ColumnType type{ColumnType::Text};
    };

    struct Table {
        std::string name;                             // operator-facing label
        std::vector<Column> columns;
        std::vector<std::vector<std::string>> rows;   // rows[r][c]; short rows read as ""
    };

    ManualDataSource() = default;
    explicit ManualDataSource(Table table);

    std::vector<Record> GetData() const override;
    std::string GetFilePath() const override { return {}; }
    std::string GetDisplayName() const override;

    Table GetTable() const;        // copy out, under m_mutex
    void SetTable(Table table);    // swap in, under m_mutex

private:
    // DataPool::Tick calls GetData() on the host's render thread every 0.25s,
    // completely independently of when the host UI thread calls SetTable() —
    // there is no per-frame handshake between them, and there doesn't need to
    // be one. The two file sources never had this problem because their state
    // lives on disk, not in the object; ScriptDataSource has the same problem
    // and solves it the same way, with its own m_dataMutex. DataPool's mutex
    // (see the threading block atop data-pool.h) protects only the pool's own
    // registry and caches — it says nothing about what a source keeps inside
    // itself, so a source with mutable internal state is on its own for
    // guarding it. This mutex is that guard: every read and write of m_table
    // takes it, so an editor keystroke landing mid-GetData() can't hand back
    // half an old row and half a new one.
    mutable std::mutex m_mutex;
    Table m_table;
};
