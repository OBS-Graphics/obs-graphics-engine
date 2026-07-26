// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "data-pool.h"

#include <iostream>

namespace {
// Cadence at which every registered source's cache is refreshed. Titles poll
// the cache every frame, so this — not the Title side — is the real fetch rate.
constexpr double kUpdateInterval = 0.25;
}  // namespace

std::string DataPool::Add(std::unique_ptr<IDataSource> source)
{
    if (!source) return {};

    const std::string id = source->GetId();

    // Prime the cache before `source` is visible to anyone else — otherwise a
    // host that Adds a source and immediately reads Data(id) to preview it
    // sees {} until the first cadence tick. Runs before m_mutex is ever taken,
    // so it can't stall Tick() on another thread even if it were slow (it
    // isn't: a ScriptDataSource's GetData() returns its worker's already-
    // cached records without blocking, and a file source's is a synchronous
    // read). Guarded against a throwing GetData() the same way Tick() is.
    std::vector<Record> initialCache;
    bool primed = false;
    try {
        initialCache = source->GetData();
        primed = true;
    } catch (const std::exception& e) {
        std::cerr << "DataPool::Add('" << id << "'): initial GetData() failed: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "DataPool::Add('" << id << "'): initial GetData() failed with an unknown exception" << std::endl;
    }

    // The replaced source (if any) is moved here and destroyed only after
    // m_mutex is released: ~ScriptDataSource joins a worker thread that can
    // block for seconds finishing an in-flight fetch, and Tick() blocks on
    // this same mutex every frame on the host's render thread.
    std::unique_ptr<IDataSource> doomed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto existing = m_sources.find(id);
        if (existing != m_sources.end()) {
            doomed = std::move(existing->second.source);
            m_sources.erase(existing);
        }

        Entry entry;
        entry.source = std::move(source);
        entry.cache = std::move(initialCache);
        // Version 0 means "never fetched"; a successful priming fetch counts
        // as the first version even when it produced no records, so a reader
        // can tell an empty-but-fetched cache from an unfetched one.
        entry.version = primed ? 1 : 0;
        entry.lastFetchFailed = !primed;
        m_sources[id] = std::move(entry);
    }
    // `doomed` destructs here, after m_mutex has been released.

    return id;
}

void DataPool::Remove(const std::string& sourceId)
{
    std::unique_ptr<IDataSource> doomed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sources.find(sourceId);
        if (it != m_sources.end()) {
            doomed = std::move(it->second.source);
            m_sources.erase(it);
        }
    }
    // `doomed` destructs here, after m_mutex has been released.
}

void DataPool::Clear()
{
    std::vector<std::unique_ptr<IDataSource>> doomed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        doomed.reserve(m_sources.size());
        for (auto& [id, entry] : m_sources)
            doomed.push_back(std::move(entry.source));
        m_sources.clear();
    }
    // Every `doomed` source destructs here, after m_mutex has been released.
}

bool DataPool::Has(const std::string& sourceId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sources.count(sourceId) != 0;
}

std::vector<std::string> DataPool::Ids() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> ids;
    ids.reserve(m_sources.size());
    for (const auto& [id, entry] : m_sources)
        ids.push_back(id);
    return ids;
}

IDataSource* DataPool::Get(const std::string& sourceId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sources.find(sourceId);
    return it == m_sources.end() ? nullptr : it->second.source.get();
}

std::vector<Record> DataPool::Data(const std::string& sourceId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sources.find(sourceId);
    return it == m_sources.end() ? std::vector<Record>{} : it->second.cache;
}

uint64_t DataPool::DataVersion(const std::string& sourceId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sources.find(sourceId);
    return it == m_sources.end() ? 0 : it->second.version;
}

bool DataPool::DataIfChanged(const std::string& sourceId, uint64_t& version, std::vector<Record>& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sources.find(sourceId);
    if (it == m_sources.end()) return false;
    if (it->second.version == version) return false;

    out = it->second.cache;
    version = it->second.version;
    return true;
}

std::vector<Record> DataPool::DataBlocking(const std::string& sourceId)
{
    IDataSource* source = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sources.find(sourceId);
        if (it == m_sources.end()) return {};
        source = it->second.source.get();
    }

    // MUST NOT hold m_mutex here: GetDataBlocking() can take seconds (a
    // network-backed ScriptDataSource) and must never stall Tick(), Add(),
    // Remove(), or another source's Data() on a different thread. It can also
    // throw for the same reason GetData() can, so it's caught here rather than
    // escaping into Title::TriggerIn.
    std::vector<Record> records;
    bool ok = true;
    try {
        records = source->GetDataBlocking();
    } catch (const std::exception& e) {
        ok = false;
        std::cerr << "DataPool::DataBlocking('" << sourceId << "'): GetDataBlocking() failed: " << e.what() << std::endl;
    } catch (...) {
        ok = false;
        std::cerr << "DataPool::DataBlocking('" << sourceId
                  << "'): GetDataBlocking() failed with an unknown exception" << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sources.find(sourceId);
        // Best-effort: only touch the cache if this exact source is still
        // registered — it may have been removed/replaced while we waited.
        if (it != m_sources.end() && it->second.source.get() == source) {
            if (ok) {
                if (it->second.cache != records || it->second.version == 0) {
                    it->second.cache = records;
                    ++it->second.version;
                }
            } else {
                records = it->second.cache;  // hand back the last good cache instead of {}
            }
        }
    }

    return records;
}

void DataPool::NotifyTriggerIn(const std::string& sourceId, const TitleRef& title, size_t recordIndex, double duration)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sources.find(sourceId);
    if (it == m_sources.end()) return;
    it->second.source->NotifyTriggerIn(title, recordIndex, duration);
}

void DataPool::NotifyTriggerOut(const std::string& sourceId, const TitleRef& title)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sources.find(sourceId);
    if (it == m_sources.end()) return;
    it->second.source->NotifyTriggerOut(title);
}

void DataPool::PublishTitleDirectory(const std::vector<TitleRef>& titles)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, entry] : m_sources)
        entry.source->SetTitleDirectory(titles);
}

std::vector<std::pair<std::string, std::vector<std::string>>> DataPool::DrainOutTriggerRequests()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::pair<std::string, std::vector<std::string>>> drained;
    for (auto& [id, entry] : m_sources) {
        auto requested = entry.source->DrainOutTriggerRequests();
        if (requested.empty()) continue;
        drained.emplace_back(id, std::move(requested));
    }
    return drained;
}

void DataPool::Tick(float dt)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& [id, entry] : m_sources) {
        entry.source->PumpEvents();

        entry.prevUpdateTimer = entry.updateTimer;
        entry.updateTimer += dt;

        // Integer-division slot compare: fires exactly once per interval
        // crossed, regardless of frame pacing.
        int prevSlot = static_cast<int>(entry.prevUpdateTimer / kUpdateInterval);
        int currSlot = static_cast<int>(entry.updateTimer / kUpdateInterval);
        if (prevSlot == currSlot) continue;

        try {
            std::vector<Record> fresh = entry.source->GetData();
            // The changed flag: only a real difference moves `version`, so a
            // Title polling every frame animates on genuine data changes only.
            if (entry.version == 0 || fresh != entry.cache) {
                entry.cache = std::move(fresh);
                ++entry.version;
            }
            if (entry.lastFetchFailed) {
                std::cerr << "DataPool: source '" << id << "' recovered" << std::endl;
                entry.lastFetchFailed = false;
            }
        } catch (const std::exception& e) {
            if (!entry.lastFetchFailed) {
                std::cerr << "DataPool: GetData() failed for source '" << id << "': " << e.what() << std::endl;
                entry.lastFetchFailed = true;
            }
        } catch (...) {
            if (!entry.lastFetchFailed) {
                std::cerr << "DataPool: GetData() failed for source '" << id
                          << "' with an unknown exception" << std::endl;
                entry.lastFetchFailed = true;
            }
        }
    }
}
