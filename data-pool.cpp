// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "data-pool.h"

#include <algorithm>

namespace {
// Same 0.25s cadence Title::Tick used to run per-title before DataPool
// existed (see title.cpp history) — now tracked per-source in Entry.
constexpr double kUpdateInterval = 0.25;
}  // namespace

void DataPool::Add(const std::string& key, std::unique_ptr<IDataSource> source)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Collect every existing binding under this key BEFORE tearing anything
    // down, so they can be transparently re-bound to the new source after —
    // the host shouldn't have to re-Bind every Title just because the
    // source behind a key was swapped out.
    struct Rebind {
        Title* title;
        std::string bindName;
        std::mutex* titleLock;
    };
    std::vector<Rebind> rebinds;
    for (auto& b : m_bindings) {
        if (b.key != key) continue;
        rebinds.push_back({b.title, b.bindName, b.titleLock});
        *b.alive = false;  // neutralize the lambda tied to the OLD source
    }
    m_bindings.erase(
        std::remove_if(m_bindings.begin(), m_bindings.end(),
                        [&](const Binding& b) { return b.key == key; }),
        m_bindings.end());

    Entry entry;
    entry.source = std::move(source);
    m_sources[key] = std::move(entry);

    for (auto& r : rebinds)
        BindLocked(r.title, key, r.bindName, r.titleLock);
}

void DataPool::Remove(const std::string& key)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& b : m_bindings)
        if (b.key == key) *b.alive = false;
    m_bindings.erase(
        std::remove_if(m_bindings.begin(), m_bindings.end(),
                        [&](const Binding& b) { return b.key == key; }),
        m_bindings.end());

    m_sources.erase(key);
}

void DataPool::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& b : m_bindings)
        *b.alive = false;
    m_bindings.clear();
    m_sources.clear();
}

bool DataPool::Has(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sources.count(key) != 0;
}

std::vector<std::string> DataPool::Keys() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> keys;
    keys.reserve(m_sources.size());
    for (const auto& [k, entry] : m_sources)
        keys.push_back(k);
    return keys;
}

IDataSource* DataPool::Get(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sources.find(key);
    return it == m_sources.end() ? nullptr : it->second.source.get();
}

void DataPool::BindLocked(Title* title, const std::string& key, std::string bindName, std::mutex* titleLock)
{
    auto it = m_sources.find(key);
    if (it == m_sources.end()) return;  // unknown key: nothing to bind to

    IDataSource* source = it->second.source.get();
    auto sourceAlive = source->AliveToken();
    auto alive = std::make_shared<std::atomic<bool>>(true);

    auto installSubscribers = [&] {
        title->onTriggerIn.push_back(
            [source, bindName, alive, sourceAlive](size_t recordIndex, double duration) {
                if (!alive->load(std::memory_order_acquire)) return;
                if (sourceAlive && !sourceAlive->load(std::memory_order_acquire)) return;
                source->NotifyTriggerIn(bindName, recordIndex, duration);
            });
        title->onTriggerOut.push_back(
            [source, bindName, alive, sourceAlive] {
                if (!alive->load(std::memory_order_acquire)) return;
                if (sourceAlive && !sourceAlive->load(std::memory_order_acquire)) return;
                source->NotifyTriggerOut(bindName);
            });
    };

    // Appending onTriggerIn/onTriggerOut is a mutation of *title — guarded
    // per the documented pool-mutex -> title-lock order.
    if (titleLock) {
        std::lock_guard<std::mutex> tl(*titleLock);
        installSubscribers();
    } else {
        installSubscribers();
    }

    Binding binding;
    binding.title = title;
    binding.key = key;
    binding.bindName = std::move(bindName);
    binding.titleLock = titleLock;
    binding.alive = std::move(alive);
    m_bindings.push_back(std::move(binding));
}

void DataPool::UnbindLocked(Title* title)
{
    for (auto& b : m_bindings)
        if (b.title == title) *b.alive = false;
    m_bindings.erase(
        std::remove_if(m_bindings.begin(), m_bindings.end(),
                        [&](const Binding& b) { return b.title == title; }),
        m_bindings.end());
}

void DataPool::Bind(Title* title, const std::string& key, std::string bindName, std::mutex* titleLock)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // A Title is a pure consumer bound to exactly one source at a time.
    UnbindLocked(title);
    BindLocked(title, key, std::move(bindName), titleLock);
}

void DataPool::Unbind(Title* title)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    UnbindLocked(title);
}

std::vector<std::string> DataPool::BoundTitleNames(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> names;
    for (const auto& b : m_bindings)
        if (b.key == key) names.push_back(b.bindName);
    return names;
}

std::vector<Record> DataPool::Data(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sources.find(key);
    return it == m_sources.end() ? std::vector<Record>{} : it->second.cache;
}

std::vector<Record> DataPool::DataBlocking(const std::string& key)
{
    IDataSource* source = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sources.find(key);
        if (it == m_sources.end()) return {};
        source = it->second.source.get();
    }

    // MUST NOT hold m_mutex here: GetDataBlocking() can take seconds (a
    // network-backed ScriptDataSource) and must never stall Tick(), Add(),
    // Remove(), or another key's Data() on a different thread.
    auto records = source->GetDataBlocking();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sources.find(key);
        // Best-effort: only cache if `key` (and this exact source) is still
        // registered — it may have been Removed/replaced while we waited.
        if (it != m_sources.end() && it->second.source.get() == source)
            it->second.cache = records;
    }

    return records;
}

void DataPool::TriggerOutLocked(Binding& binding)
{
    if (binding.titleLock) {
        std::lock_guard<std::mutex> tl(*binding.titleLock);
        binding.title->TriggerOut();
    } else {
        binding.title->TriggerOut();
    }
}

bool DataPool::AnyBoundTitleVisibleLocked(const std::string& key) const
{
    for (const auto& b : m_bindings) {
        if (b.key != key) continue;

        bool visible;
        if (b.titleLock) {
            std::lock_guard<std::mutex> tl(*b.titleLock);
            visible = (b.title->state == TitleState::Visible);
        } else {
            visible = (b.title->state == TitleState::Visible);
        }
        if (visible) return true;
    }
    return false;
}

void DataPool::Tick(float dt)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // (a) Pump every source, then drain and apply any script-originated
    // trigger_out(bindName) requests by mapping each bind name back to the
    // Title(s) bound to that source under the same key. Runs unconditionally
    // (regardless of any binding's visibility), same as the old
    // Title::Tick's unconditional PumpEvents() call.
    for (auto& [key, entry] : m_sources) {
        entry.source->PumpEvents();

        for (const std::string& requestedName : entry.source->DrainOutTriggerRequests()) {
            for (auto& binding : m_bindings) {
                if (binding.key != key) continue;
                // Empty string is the "every title bound to this source"
                // sentinel (Lua's bare trigger_out()).
                if (!requestedName.empty() && binding.bindName != requestedName) continue;
                TriggerOutLocked(binding);
            }
        }
    }

    // (b) Refresh each source's cache on the fixed 0.25s cadence, but only
    // while at least one bound Title is Visible — see the Tick() doc
    // comment in data-pool.h for why this mirrors the old per-Title gating.
    for (auto& [key, entry] : m_sources) {
        entry.refreshedThisTick = false;
        if (!AnyBoundTitleVisibleLocked(key)) continue;

        entry.prevUpdateTimer = entry.updateTimer;
        entry.updateTimer += dt;

        int prevSlot = static_cast<int>(entry.prevUpdateTimer / kUpdateInterval);
        int currSlot = static_cast<int>(entry.updateTimer / kUpdateInterval);
        entry.refreshedThisTick = (prevSlot != currSlot);
        if (entry.refreshedThisTick)
            entry.cache = entry.source->GetData();
    }

    // (c) Push refreshed records into every Visible binding.
    for (auto& binding : m_bindings) {
        auto it = m_sources.find(binding.key);
        if (it == m_sources.end() || !it->second.refreshedThisTick) continue;

        const std::vector<Record>& records = it->second.cache;
        if (binding.titleLock) {
            std::lock_guard<std::mutex> tl(*binding.titleLock);
            if (binding.title->state == TitleState::Visible)
                binding.title->UpdateData(records, /*instant=*/false);
        } else {
            if (binding.title->state == TitleState::Visible)
                binding.title->UpdateData(records, /*instant=*/false);
        }
    }
}
