// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include <sol/sol.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "data-source.h"

// Runs a Lua script as an IDataSource, on its own dedicated worker thread.
//
// The worker thread owns `L` (and every sol object derived from it)
// exclusively for the lifetime of the instance — nothing on that state is
// ever touched from the host/tick thread. `GetData()`/`GetDataBlocking()`
// (called from the host thread) only ever touch small thread-safe caches;
// see script.cpp for the full cross-thread contract.
class ScriptDataSource : public IDataSource {
public:
    std::string scriptFilePath;

    explicit ScriptDataSource(const std::string& path);
    ~ScriptDataSource() override;
    ScriptDataSource(const ScriptDataSource&) = delete;
    ScriptDataSource& operator=(const ScriptDataSource&) = delete;

    std::vector<Record> GetData() const override;
    std::vector<Record> GetDataBlocking() const override;
    std::string GetFilePath() const override
    {
        return scriptFilePath;
    }

    std::vector<std::string> DrainOutTriggerRequests() override;
    void NotifyTriggerIn(const TitleRef& title, size_t recordIndex, double duration) override;
    void NotifyTriggerOut(const TitleRef& title) override;
    void SetTitleDirectory(std::vector<TitleRef> titles) override;

    // Set if the script failed to load/parse; safe to call from any thread.
    bool LoadFailed() const { return m_loadFailed.load(std::memory_order_acquire); }
    std::string GetLoadError() const;

private:
    // ── worker-thread-owned; never touched from the host/tick thread ──────
    sol::state L;
    sol::protected_function m_getData;
    sol::protected_function m_onTriggerIn;
    sol::protected_function m_onTriggerOut;
    double m_pollInterval{0.25};  // seconds; from optional Lua global `_poll_interval`

    void WorkerMain();
    std::vector<Record> RunGetData();
    void FetchAndCacheData();

    // `{ id = ..., name = ... }` — the only shape a Title ever takes in Lua.
    // Worker thread only (it touches L).
    sol::table MakeTitleHandle(const TitleRef& title);

    // Lua-bound global `trigger_out(title)` (script -> host). Only ever
    // called on the worker thread; must not touch a Title directly (see
    // script.cpp) — a source only ever holds TitleRef copies, handed to it by
    // Scene. `titleId` is a Title uuid, or empty for the bare `trigger_out()`
    // form (the "every title reading this source" sentinel — see
    // DrainOutTriggerRequests). There is deliberately no `trigger_in`
    // counterpart: whether a title is on-screen at all is the host's call, so
    // a script asking to be *shown* is not something the engine can honour
    // meaningfully. Scripts still learn about every show/hide through
    // _on_trigger_in(title, ...)/_on_trigger_out(title).
    void TriggerOut(const std::string& titleId);

    // Lua-bound `scene.find_titles(name)` / `scene.titles()` (worker thread):
    // reads the directory Scene last published (see SetTitleDirectory).
    std::vector<TitleRef> TitleDirectory() const;

    // ── cross-thread: last-fetched data, with a generation counter so
    //    GetDataBlocking() can wait for a specific fresh fetch to land ──────
    mutable std::mutex m_dataMutex;
    mutable std::condition_variable m_dataCv;
    mutable std::vector<Record> m_cachedRecords;
    mutable uint64_t m_dataGeneration{0};

    // Shape of every inbound cross-thread message (host -> script): a FIFO
    // job-queue element, with `FetchRequest` folded in here rather than
    // tracked as a separate flag. `title` is which Title a Kind::In/Out job
    // is for, forwarded as the leading `{id=, name=}` table argument to
    // _on_trigger_in/_on_trigger_out — unused for FetchRequest.
    struct TriggerRequest {
        enum class Kind { None, In, Out, FetchRequest } kind{Kind::None};
        TitleRef title;
        size_t recordIndex{0};
        double duration{-1.0};
    };

    // ── cross-thread: pending outbound trigger_out(title) requests
    //    (script -> host), drained via DrainOutTriggerRequests() and applied
    //    by Scene::Tick — this source only ever queues them. A vector rather
    //    than a single flag: more than one distinct request (e.g. two
    //    different titles, or one title and the bare all-titles form) can be
    //    pending at once between two drains.
    mutable std::mutex m_outTriggerMutex;
    mutable std::vector<std::string> m_pendingOutTriggers;

    // ── cross-thread: the Scene's Title directory (host -> script), pushed
    //    by Scene::Tick whenever it changes and read by the Lua `scene` table
    //    on the worker thread ─────────────────────────────────────────────
    mutable std::mutex m_titleDirMutex;
    std::vector<TitleRef> m_titleDirectory;

    // ── cross-thread: inbound job queue (host -> script), consumed by the
    //    worker thread; a FetchRequest job requests an immediate out-of-band
    //    fetch for GetDataBlocking() ───────────────────────────────────────
    mutable std::mutex m_jobMutex;
    mutable std::condition_variable m_jobCv;
    mutable std::deque<TriggerRequest> m_triggerJobs;
    void EnqueueTriggerJob(TriggerRequest job) const;

    // ── load-failure reporting ──────────────────────────────────────────
    std::atomic<bool> m_loadFailed{false};
    mutable std::mutex m_loadErrorMutex;
    std::string m_loadError;

    // ── lifecycle ────────────────────────────────────────────────────────
    std::thread m_worker;
    std::atomic<bool> m_stopRequested{false};
};
