// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "data-pool.h"

#include <algorithm>
#include <iostream>
#include <unordered_set>

namespace {
// Same 0.25s cadence Title::Tick used to run per-title before DataPool
// existed (see title.cpp history) — now tracked per-source in Entry.
constexpr double kUpdateInterval = 0.25;
}  // namespace

void DataPool::Add(const std::string& key, std::unique_ptr<IDataSource> source)
{
    // Defect A: prime the cache before `source` is visible to anyone else —
    // otherwise a host that Adds a source and immediately calls Data(key) to
    // preview it sees {} until some bound Title goes Visible (Tick's step
    // (b) is gated on that). This deliberately runs before m_mutex is ever
    // taken, so it can't stall Tick()/Bind()/etc. on another thread even if
    // it were slow (it isn't: a ScriptDataSource's GetData() returns its
    // worker's already-cached records non-blockingly, and a file source's
    // GetData() is a synchronous read — GetDataBlocking() is for the
    // Hidden-triggered instant-apply path, not registration). Guarded
    // against a throwing GetData() the same way Tick()'s step (b) is
    // (defect C) — a missing/malformed file shouldn't take Add() down with it.
    std::vector<Record> initialCache;
    if (source) {
        try {
            initialCache = source->GetData();
        } catch (const std::exception& e) {
            std::cerr << "DataPool::Add('" << key << "'): initial GetData() failed: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "DataPool::Add('" << key << "'): initial GetData() failed with an unknown exception" << std::endl;
        }
    }

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
    // Defect B: the OLD source (if any) is moved here and destroyed only
    // after m_mutex is released below — never under the lock, since
    // ~ScriptDataSource joins a worker thread that can block for seconds
    // finishing an in-flight fetch, and Tick() blocks on this same mutex
    // every frame on the host's render thread.
    std::unique_ptr<IDataSource> doomed;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& b : m_bindings) {
            if (b.key != key) continue;
            rebinds.push_back({b.title, b.bindName, b.titleLock});
        }
        m_bindings.erase(
            std::remove_if(m_bindings.begin(), m_bindings.end(),
                            [&](const Binding& b) { return b.key == key; }),
            m_bindings.end());

        auto existing = m_sources.find(key);
        if (existing != m_sources.end()) {
            doomed = std::move(existing->second.source);
            m_sources.erase(existing);
        }

        Entry entry;
        entry.source = std::move(source);
        entry.cache = std::move(initialCache);
        m_sources[key] = std::move(entry);

        for (auto& r : rebinds)
            BindLocked(r.title, key, r.bindName, r.titleLock);
    }
    // `doomed` (the replaced source, if any) is destroyed here, after
    // m_mutex has been released.
}

void DataPool::Remove(const std::string& key)
{
    // Defect B: destroy the removed source only after m_mutex is released —
    // see the identical note in Add().
    std::unique_ptr<IDataSource> doomed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& b : m_bindings) {
            if (b.key != key) continue;
            NeutralizeTriggerTargetLocked(b.title, b.titleLock);
        }
        m_bindings.erase(
            std::remove_if(m_bindings.begin(), m_bindings.end(),
                            [&](const Binding& b) { return b.key == key; }),
            m_bindings.end());

        auto it = m_sources.find(key);
        if (it != m_sources.end()) {
            doomed = std::move(it->second.source);
            m_sources.erase(it);
        }
    }
    // `doomed` destructs here, after m_mutex has been released.
}

void DataPool::Clear()
{
    // Defect B: destroy every source only after m_mutex is released — see
    // the identical note in Add().
    std::vector<std::unique_ptr<IDataSource>> doomed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& b : m_bindings)
            NeutralizeTriggerTargetLocked(b.title, b.titleLock);
        m_bindings.clear();

        doomed.reserve(m_sources.size());
        for (auto& [k, entry] : m_sources)
            doomed.push_back(std::move(entry.source));
        m_sources.clear();
    }
    // Every `doomed` source destructs here, after m_mutex has been released.
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

void DataPool::TriggerInThunk::operator()(size_t recordIndex, double duration) const
{
    IDataSource* src;
    std::string name;
    std::shared_ptr<const std::atomic<bool>> alive;
    {
        std::lock_guard<std::mutex> tl(target->mutex);
        src = target->source;
        name = target->bindName;
        alive = target->sourceAlive;
    }
    if (!src) return;  // currently unbound
    if (alive && !alive->load(std::memory_order_acquire)) return;
    src->NotifyTriggerIn(name, recordIndex, duration);
}

void DataPool::TriggerOutThunk::operator()() const
{
    IDataSource* src;
    std::string name;
    std::shared_ptr<const std::atomic<bool>> alive;
    {
        std::lock_guard<std::mutex> tl(target->mutex);
        src = target->source;
        name = target->bindName;
        alive = target->sourceAlive;
    }
    if (!src) return;  // currently unbound
    if (alive && !alive->load(std::memory_order_acquire)) return;
    src->NotifyTriggerOut(name);
}

std::shared_ptr<DataPool::TriggerTarget> DataPool::FindOrInstallTriggerTargetLocked(Title* title, std::mutex* titleLock)
{
    auto findOrInstall = [&]() -> std::shared_ptr<TriggerTarget> {
        // Defect D: look for a TriggerOutThunk this Title already carries
        // (installed by a previous BindLocked on this exact object) instead
        // of consulting any DataPool-side registry — see TriggerTarget's
        // doc comment for why a registry keyed by Title* would be unsafe.
        for (auto& cb : title->onTriggerOut) {
            if (auto* thunk = cb.target<TriggerOutThunk>())
                return thunk->target;
        }

        auto target = std::make_shared<TriggerTarget>();
        title->onTriggerIn.push_back(TriggerInThunk{target});
        title->onTriggerOut.push_back(TriggerOutThunk{target});
        return target;
    };

    // Scanning/appending onTriggerIn/onTriggerOut is a read/mutation of
    // *title — guarded per the documented pool-mutex -> title-lock order.
    if (titleLock) {
        std::lock_guard<std::mutex> tl(*titleLock);
        return findOrInstall();
    }
    return findOrInstall();
}

void DataPool::NeutralizeTriggerTargetLocked(Title* title, std::mutex* titleLock)
{
    auto retarget = [&] {
        for (auto& cb : title->onTriggerOut) {
            if (auto* thunk = cb.target<TriggerOutThunk>()) {
                std::lock_guard<std::mutex> tl(thunk->target->mutex);
                thunk->target->source = nullptr;
                thunk->target->sourceAlive.reset();
                return;
            }
        }
    };

    if (titleLock) {
        std::lock_guard<std::mutex> tl(*titleLock);
        retarget();
    } else {
        retarget();
    }
}

void DataPool::BindLocked(Title* title, const std::string& key, std::string bindName, std::mutex* titleLock)
{
    auto it = m_sources.find(key);
    if (it == m_sources.end()) return;  // unknown key: nothing to bind to

    IDataSource* source = it->second.source.get();
    auto sourceAlive = source->AliveToken();

    // Defect D: at most one onTriggerIn/onTriggerOut subscriber pair is ever
    // installed on a given Title, no matter how many times it's Bind()'d/
    // Unbind()'d/rebound — this finds the existing pair's redirect cell (if
    // this Title already has one) instead of installing another.
    auto target = FindOrInstallTriggerTargetLocked(title, titleLock);

    {
        std::lock_guard<std::mutex> tl(target->mutex);
        target->source = source;
        target->bindName = bindName;
        target->sourceAlive = sourceAlive;
    }

    Binding binding;
    binding.title = title;
    binding.key = key;
    binding.bindName = std::move(bindName);
    binding.titleLock = titleLock;
    m_bindings.push_back(std::move(binding));
}

void DataPool::UnbindLocked(Title* title)
{
    // Defect D: don't touch title->onTriggerIn/onTriggerOut — just retarget
    // this Title's persistent subscriber (if it has one) to nothing, so it
    // becomes an inert no-op instead of being (impossibly, given no removal
    // API) erased. Reuses whatever titleLock this Title's current binding
    // (if any) was registered with, for the same read/mutate-under-titleLock
    // discipline BindLocked follows.
    std::mutex* titleLock = nullptr;
    for (auto& b : m_bindings) {
        if (b.title == title) {
            titleLock = b.titleLock;
            break;
        }
    }
    NeutralizeTriggerTargetLocked(title, titleLock);

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
    //
    // Defect C: GetDataBlocking() can throw for the same reason GetData()
    // can (a file source reading a missing/malformed file) — caught here so
    // it never escapes into the caller.
    std::vector<Record> records;
    bool ok = true;
    try {
        records = source->GetDataBlocking();
    } catch (const std::exception& e) {
        ok = false;
        std::cerr << "DataPool::DataBlocking('" << key << "'): GetDataBlocking() failed: " << e.what() << std::endl;
    } catch (...) {
        ok = false;
        std::cerr << "DataPool::DataBlocking('" << key << "'): GetDataBlocking() failed with an unknown exception" << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sources.find(key);
        // Best-effort: only touch the cache if `key` (and this exact
        // source) is still registered — it may have been Removed/replaced
        // while we waited.
        if (it != m_sources.end() && it->second.source.get() == source) {
            if (ok)
                it->second.cache = records;
            else
                records = it->second.cache;  // hand back the last good cache instead of {}
        }
    }

    return records;
}

void DataPool::TriggerOutLocked(Binding& binding)
{
    auto fire = [&] {
        // Defect E: a Title that's already Hidden or on its way out must not
        // have TriggerOut() re-applied — it unconditionally resets
        // state/timer and re-fires every subscriber, which would replay the
        // out-animation on an already-hidden Title and double-relay
        // _on_trigger_out to a script when a single drain names the same
        // Title more than once (the caller in Tick() also deduplicates
        // within one drain; this is the second half of that fix).
        if (binding.title->state == TitleState::Hidden ||
            binding.title->state == TitleState::AnimatingOut)
            return;
        binding.title->TriggerOut();
    };

    if (binding.titleLock) {
        std::lock_guard<std::mutex> tl(*binding.titleLock);
        fire();
    } else {
        fire();
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

        auto requested = entry.source->DrainOutTriggerRequests();
        if (requested.empty()) continue;

        // Defect E: de-duplicate within this one drain so a script that
        // asks for both a named and the bare-sentinel trigger_out() in the
        // same _get_data pass doesn't fire the same bound Title's
        // TriggerOut() twice (TriggerOutLocked itself skips a Title that's
        // already Hidden/AnimatingOut, covering the other half of this).
        std::unordered_set<Title*> firedThisDrain;
        for (const std::string& requestedName : requested) {
            for (auto& binding : m_bindings) {
                if (binding.key != key) continue;
                // Empty string is the "every title bound to this source"
                // sentinel (Lua's bare trigger_out()).
                if (!requestedName.empty() && binding.bindName != requestedName) continue;
                if (!firedThisDrain.insert(binding.title).second) continue;
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
        if (!entry.refreshedThisTick) continue;

        // Defect C: GetData() can throw (a file source reading a
        // missing/malformed file — see JsonFileDataSource/CsvFileDataSource
        // in data-source.cpp). Tick() runs on the host's render thread,
        // called from C code (libobs), so an escaping exception here would
        // be std::terminate. On failure, keep the previous cache (a
        // transient read error shouldn't blank a Title that's currently on
        // screen) and log only on the failure/recovery edge, not every
        // 0.25s tick.
        try {
            entry.cache = entry.source->GetData();
            if (entry.lastFetchFailed) {
                std::cerr << "DataPool: source '" << key << "' recovered" << std::endl;
                entry.lastFetchFailed = false;
            }
        } catch (const std::exception& e) {
            if (!entry.lastFetchFailed) {
                std::cerr << "DataPool: GetData() failed for source '" << key << "': " << e.what() << std::endl;
                entry.lastFetchFailed = true;
            }
        } catch (...) {
            if (!entry.lastFetchFailed) {
                std::cerr << "DataPool: GetData() failed for source '" << key
                           << "' with an unknown exception" << std::endl;
                entry.lastFetchFailed = true;
            }
        }
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
