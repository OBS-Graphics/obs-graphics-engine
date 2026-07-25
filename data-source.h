// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using Record = std::unordered_map<std::string, std::string>;

// A source of records that `DataPool` (data-pool.h) polls and hands out to
// any number of `Title`s bound to it. A source knows nothing about which
// Title(s) consume it, nor even how many there are — that mapping (and its
// lifetime) is owned entirely by DataPool. This is what lets several Titles
// safely share one instance instead of each needing its own copy (e.g. a
// weather feed backing both a corner ticker and a full-screen graphic).
struct IDataSource {
    virtual ~IDataSource() = default;
    virtual std::vector<Record> GetData() const = 0;
    virtual std::string GetFilePath() const = 0;

    // Like GetData(), but for sources that fetch asynchronously (e.g. a
    // worker-thread-backed ScriptDataSource), waits for a fresh fetch to
    // complete (bounded) instead of returning whatever's currently cached.
    // Used only for the Hidden-triggered instant-apply path (the caller
    // fetches via this before calling Title::TriggerIn with the records),
    // where showing stale/blank data on first appearance would be wrong.
    // Synchronous sources (file-backed) are already always fresh, so the
    // default just delegates to GetData().
    virtual std::vector<Record> GetDataBlocking() const { return GetData(); }

    // Per-tick housekeeping hook, called by DataPool::Tick on every source
    // every frame regardless of any binding's Title state (mirrors the old
    // per-Title unconditional call this replaces). Default: nothing to do.
    // Must not block. Note this no longer *applies* anything itself (e.g. a
    // script's own trigger_out()) — see DrainOutTriggerRequests(), which is
    // what a caller (DataPool) actually applies.
    virtual void PumpEvents() const {}

    // Drains script-originated trigger_out(...) requests accumulated since
    // the last drain (DataPool::Tick calls this once per source per tick).
    // Each returned string is a *bind name* (see DataPool::Bind) identifying
    // one bound title to hide; the empty-string sentinel means "every title
    // currently bound to this source" (Lua's bare trigger_out()). Default:
    // nothing to drain — only ScriptDataSource produces these.
    virtual std::vector<std::string> DrainOutTriggerRequests() { return {}; }

    // Called by DataPool once per binding, whenever that binding's Title
    // fires onTriggerIn/onTriggerOut — from any origin (a host call, the
    // `duration` timeout, or this same source's own trigger_out()).
    // `bindName` identifies which binding fired, since one source can be
    // bound to many titles at once. Default: no-op — only sources that care
    // about a title's show/hide (e.g. ScriptDataSource, to relay into
    // _on_trigger_in(titleId, ...)/_on_trigger_out(titleId)) need to
    // override these.
    virtual void NotifyTriggerIn(const std::string& bindName, size_t recordIndex, double duration) {}
    virtual void NotifyTriggerOut(const std::string& bindName) {}

    // Optional liveness token. DataPool::Bind installs a callback on the
    // bound Title that calls back into this source (NotifyTriggerIn/Out);
    // DataPool guarantees it unbinds (and neutralizes that callback) before
    // ever destroying a source, but a source that has its own good reason to
    // distrust that ordering (or simply wants a second, source-owned
    // guarantee) can hand back a shared flag it flips to false at the very
    // start of its own destructor — DataPool checks it, when present, before
    // every NotifyTriggerIn/Out call. Default: none; DataPool's own
    // bookkeeping is sufficient for synchronous sources.
    virtual std::shared_ptr<const std::atomic<bool>> AliveToken() const { return nullptr; }
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
