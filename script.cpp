// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "script.h"

#include <chrono>
#include <iostream>
#include <optional>

#include "lua_http.h"
#include "lua_json.h"
#include "lua_xml.h"

namespace {
// Bounds how long GetDataBlocking() waits for a whole _get_data() run to
// complete. Deliberately separate from lua_http::kRequestTimeout, which caps
// a single HTTP call: a _get_data() that issues several sequential http.*
// calls (each individually within its own budget) can legitimately take
// longer than any one of them.
constexpr std::chrono::seconds kInstantFetchTimeout{30};

// Resolves whatever Lua passed to trigger_out(...) into a Title uuid:
//  - nil / no argument  -> "" (the "every title reading this source" sentinel)
//  - a `{id=, name=}` handle from scene.find_titles/scene.titles -> its id
//  - a raw uuid string  -> itself
// Anything else (or a table with no string `id`) yields no value, so the
// caller can complain instead of silently hiding every title on the source.
std::optional<std::string> TitleIdFromLua(const sol::object& title)
{
    switch (title.get_type()) {
        case sol::type::none:
        case sol::type::lua_nil:
            return std::string();
        case sol::type::string:
            return title.as<std::string>();
        case sol::type::table: {
            sol::optional<std::string> id = title.as<sol::table>()["id"];
            if (id) return *id;
            return std::nullopt;
        }
        default:
            return std::nullopt;
    }
}
}  // namespace

ScriptDataSource::ScriptDataSource(const std::string& path)
    : scriptFilePath(path)
{
    m_worker = std::thread(&ScriptDataSource::WorkerMain, this);
}

ScriptDataSource::~ScriptDataSource()
{
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

        // Registered after safe_script_file, so these are callable from
        // _get_data/_on_trigger_* but not at script load time.
        L.set_function("trigger_out", [this](sol::object title) {
            if (auto id = TitleIdFromLua(title)) {
                TriggerOut(*id);
                return;
            }
            std::cerr << "trigger_out: expected a title handle (from scene.find_titles), "
                         "a title id string, or no argument" << std::endl;
        });

        sol::table sceneTable = L.create_table();
        sceneTable.set_function("titles", [this]() {
            sol::table out = L.create_table();
            for (const TitleRef& t : TitleDirectory())
                out.add(MakeTitleHandle(t));
            return out;
        });
        sceneTable.set_function("find_titles", [this](const std::string& name) {
            sol::table out = L.create_table();
            for (const TitleRef& t : TitleDirectory())
                if (t.name == name) out.add(MakeTitleHandle(t));
            return out;
        });
        L["scene"] = sceneTable;

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
                auto r = m_onTriggerIn(MakeTitleHandle(job.title), job.recordIndex, job.duration);
                if (!r.valid()) {
                    sol::error e = r;
                    std::cerr << e.what() << std::endl;
                }
            } else if (job.kind == TriggerRequest::Kind::Out && m_onTriggerOut.valid()) {
                auto r = m_onTriggerOut(MakeTitleHandle(job.title));
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

std::vector<Record> ScriptDataSource::GetData() const
{
    std::lock_guard<std::mutex> lock(m_dataMutex);
    return m_cachedRecords;
}

std::vector<Record> ScriptDataSource::GetDataBlocking() const
{
    // If the worker already gave up (bad path, Lua syntax error, etc.) it has
    // exited for good — see WorkerMain's catch block — so no thread will ever
    // drain a FetchRequest job or bump m_dataGeneration. Enqueueing one anyway
    // would burn the full kInstantFetchTimeout on every call, forever. Return
    // the (permanently empty) cache immediately instead.
    if (m_loadFailed.load(std::memory_order_acquire))
        return GetData();

    std::unique_lock<std::mutex> lock(m_dataMutex);
    uint64_t startGen = m_dataGeneration;
    lock.unlock();

    EnqueueTriggerJob({TriggerRequest::Kind::FetchRequest, TitleRef{}, 0, -1.0});

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

std::vector<std::string> ScriptDataSource::DrainOutTriggerRequests()
{
    std::lock_guard<std::mutex> lock(m_outTriggerMutex);
    std::vector<std::string> drained;
    drained.swap(m_pendingOutTriggers);
    return drained;
}

void ScriptDataSource::NotifyTriggerIn(const TitleRef& title, size_t recordIndex, double duration)
{
    EnqueueTriggerJob({TriggerRequest::Kind::In, title, recordIndex, duration});
}

void ScriptDataSource::NotifyTriggerOut(const TitleRef& title)
{
    EnqueueTriggerJob({TriggerRequest::Kind::Out, title, 0, -1.0});
}

void ScriptDataSource::TriggerOut(const std::string& titleId)
{
    std::lock_guard<std::mutex> lock(m_outTriggerMutex);
    m_pendingOutTriggers.push_back(titleId);
}

void ScriptDataSource::SetTitleDirectory(std::vector<TitleRef> titles)
{
    std::lock_guard<std::mutex> lock(m_titleDirMutex);
    m_titleDirectory = std::move(titles);
}

std::vector<TitleRef> ScriptDataSource::TitleDirectory() const
{
    std::lock_guard<std::mutex> lock(m_titleDirMutex);
    return m_titleDirectory;
}

sol::table ScriptDataSource::MakeTitleHandle(const TitleRef& title)
{
    return L.create_table_with("id", title.id, "name", title.name);
}
