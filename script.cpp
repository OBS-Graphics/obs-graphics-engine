// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "script.h"

#include <chrono>
#include <iostream>
#include <optional>

#include "lua_http.h"
#include "lua_json.h"
#include "lua_xml.h"
#include "title.h"

namespace {
// Bounds how long GetDataBlocking() waits for a whole _get_data() run to
// complete. Deliberately separate from lua_http::kRequestTimeout, which caps
// a single HTTP call: a _get_data() that issues several sequential http.*
// calls (each individually within its own budget) can legitimately take
// longer than any one of them.
constexpr std::chrono::seconds kInstantFetchTimeout{30};
}  // namespace

ScriptDataSource::ScriptDataSource(const std::string& path)
    : scriptFilePath(path)
{
    m_worker = std::thread(&ScriptDataSource::WorkerMain, this);
}

ScriptDataSource::~ScriptDataSource()
{
    // Neutralize any Title-held onTriggerIn/onTriggerOut lambda immediately,
    // before signaling/joining, so a callback firing mid-teardown is a
    // guaranteed-safe no-op rather than a use-after-free.
    m_alive->store(false, std::memory_order_release);

    m_stopRequested.store(true, std::memory_order_release);
    m_jobCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void ScriptDataSource::WorkerMain()
{
    try {
        L.open_libraries(
            sol::lib::base,
            sol::lib::bit32,
            sol::lib::coroutine,
            sol::lib::io,
            sol::lib::math,
            sol::lib::os,
            sol::lib::package,
            sol::lib::string,
            sol::lib::table,
            sol::lib::utf8
        );
        lua_http::Register(L);
        lua_json::Register(L);
        lua_xml::Register(L);
        L.safe_script_file(scriptFilePath);

        m_getData = L["_get_data"];
        m_onTriggerIn = L["_on_trigger_in"];
        m_onTriggerOut = L["_on_trigger_out"];

        L.set_function("trigger_out", [this] {
            TriggerOut();
        });

        sol::optional<double> interval = L["_poll_interval"];
        m_pollInterval = interval.value_or(0.25);
    } catch (const sol::error& e) {
        std::cerr << "ScriptDataSource: failed to load '" << scriptFilePath << "': " << e.what() << std::endl;
        {
            std::lock_guard<std::mutex> lock(m_loadErrorMutex);
            m_loadError = e.what();
        }
        m_loadFailed.store(true, std::memory_order_release);
        return;  // worker exits; destructor's join() still works, data stays empty forever
    }

    FetchAndCacheData();  // startup fetch, before entering the schedule loop

    auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(m_pollInterval)
    );
    auto nextTick = std::chrono::steady_clock::now() + interval;

    for (;;) {
        std::deque<TriggerRequest> jobs;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait_until(lock, nextTick, [&] {
                return m_stopRequested.load(std::memory_order_acquire) || !m_triggerJobs.empty();
            });
            if (m_stopRequested.load(std::memory_order_acquire))
                return;
            jobs.swap(m_triggerJobs);
        }

        // Apply host -> script trigger callbacks and note any out-of-band
        // fetch request. Safe: only ever runs here, on L's owning thread.
        bool fetchRequested = false;
        for (auto& job : jobs) {
            if (job.kind == TriggerRequest::Kind::In && m_onTriggerIn.valid()) {
                auto r = m_onTriggerIn(job.recordIndex, job.duration);
                if (!r.valid()) {
                    sol::error e = r;
                    std::cerr << e.what() << std::endl;
                }
            } else if (job.kind == TriggerRequest::Kind::Out && m_onTriggerOut.valid()) {
                auto r = m_onTriggerOut();
                if (!r.valid()) {
                    sol::error e = r;
                    std::cerr << e.what() << std::endl;
                }
            } else if (job.kind == TriggerRequest::Kind::FetchRequest) {
                fetchRequested = true;
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (fetchRequested) {
            // Out-of-band immediate fetch for GetDataBlocking(). Resync the
            // schedule so the next periodic poll doesn't fire again right away.
            FetchAndCacheData();
            now = std::chrono::steady_clock::now();
            nextTick = now + interval;
        } else if (now >= nextTick) {
            FetchAndCacheData();
            do {
                nextTick += interval;
            } while (nextTick <= now);
        }
        // else: woke early only because jobs arrived; loop back to the same nextTick.
    }
}

std::vector<Record> ScriptDataSource::RunGetData()
{
    if (!m_getData) return {};

    auto result = m_getData();
    if (!result.valid()) {
        sol::error err = result;
        std::cerr << err.what() << std::endl;
        return {};
    }

    if (result.get_type() != sol::type::table) {
        sol::error err = result;
        std::cerr << "Data must be a table (list of objects)" << std::endl;
        return {};
    }

    sol::table table = result;
    std::vector<Record> records;
    records.reserve(table.size());

    for (auto& [i, rowValue] : table) {
        sol::table row = rowValue.as<sol::table>();
        Record rec{};
        for (auto& [k, v] : row) {
            std::string key = k.as<std::string>();
            switch (v.get_type()) {
                case sol::type::number:
                    rec[key] = v.is<int>()
                        ? std::to_string(v.as<int>())
                        : std::to_string(v.as<double>());
                    break;
                case sol::type::string:
                    rec[key] = v.as<std::string>();
                    break;
                case sol::type::boolean:
                    rec[key] = v.as<bool>() ? "true" : "false";
                    break;
                default:
                    break;
            }
        }
        records.push_back(std::move(rec));
    }

    return records;
}

void ScriptDataSource::FetchAndCacheData()
{
    auto records = RunGetData();
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_cachedRecords = std::move(records);
        ++m_dataGeneration;
    }
    m_dataCv.notify_all();
}

void ScriptDataSource::DrainPendingOutTrigger() const
{
    if (m_pendingOutTrigger.exchange(false, std::memory_order_acq_rel) && m_owner)
        m_owner->TriggerOut();
}

std::vector<Record> ScriptDataSource::GetData() const
{
    DrainPendingOutTrigger();

    std::lock_guard<std::mutex> lock(m_dataMutex);
    return m_cachedRecords;
}

std::vector<Record> ScriptDataSource::GetDataBlocking() const
{
    DrainPendingOutTrigger();

    std::unique_lock<std::mutex> lock(m_dataMutex);
    uint64_t startGen = m_dataGeneration;
    lock.unlock();

    EnqueueTriggerJob({TriggerRequest::Kind::FetchRequest, 0, -1.0});

    lock.lock();
    m_dataCv.wait_for(lock, kInstantFetchTimeout, [&] {
        return m_dataGeneration != startGen;
    });
    return m_cachedRecords;
}

std::string ScriptDataSource::GetLoadError() const
{
    std::lock_guard<std::mutex> lock(m_loadErrorMutex);
    return m_loadError;
}

void ScriptDataSource::EnqueueTriggerJob(TriggerRequest job) const
{
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_triggerJobs.push_back(std::move(job));
    }
    m_jobCv.notify_one();
}

void ScriptDataSource::TriggerOut()
{
    m_pendingOutTrigger.store(true, std::memory_order_release);
}

void ScriptDataSource::SetOwner(Title* owner)
{
    if (owner == m_owner) return;
    IDataSource::SetOwner(owner);
    if (!owner) return;

    auto alive = m_alive;
    owner->onTriggerIn.push_back([this, alive](size_t recordIndex, double duration) {
        if (!alive->load(std::memory_order_acquire)) return;
        EnqueueTriggerJob({TriggerRequest::Kind::In, recordIndex, duration});
    });
    owner->onTriggerOut.push_back([this, alive] {
        if (!alive->load(std::memory_order_acquire)) return;
        EnqueueTriggerJob({TriggerRequest::Kind::Out, 0, -1.0});
    });
}
