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

    void SetOwner(Title* owner) override;
    void PumpEvents() const override { DrainPendingOutTrigger(); }

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

    // Lua-bound globals (script -> host). Only ever called on the worker
    // thread; must not touch Title directly (see script.cpp).
    void TriggerIn(size_t recordIndex = 0, double duration = -1.0);
    void TriggerOut();

    // ── cross-thread: last-fetched data, with a generation counter so
    //    GetDataBlocking() can wait for a specific fresh fetch to land ──────
    mutable std::mutex m_dataMutex;
    mutable std::condition_variable m_dataCv;
    mutable std::vector<Record> m_cachedRecords;
    mutable uint64_t m_dataGeneration{0};

    // Shared shape for every trigger-related cross-thread message: used both
    // as a FIFO job-queue element (host -> script; `FetchRequest` is folded
    // in here too, rather than tracked as a separate flag) and as a
    // single-slot mailbox (script -> host; only that direction ever uses
    // `None` as "empty").
    struct TriggerRequest {
        enum class Kind { None, In, Out, FetchRequest } kind{Kind::None};
        size_t recordIndex{0};
        double duration{-1.0};
    };

    // ── cross-thread: pending outbound trigger request (script -> host),
    //    drained and applied inside GetData()/GetDataBlocking()/PumpEvents() ──
    mutable std::mutex m_pendingOutMutex;
    mutable TriggerRequest m_pendingOut;
    void DrainPendingOutTrigger() const;

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

    // Neutralizes Title-held onTriggerIn/onTriggerOut lambdas the instant
    // destruction begins, so a stale callback firing mid-teardown becomes a
    // safe no-op instead of touching a half-destroyed ScriptDataSource.
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};
