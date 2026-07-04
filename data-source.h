// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

using Record = std::unordered_map<std::string, std::string>;

struct Title;
struct IDataSource {
    virtual ~IDataSource() = default;
    virtual std::vector<Record> GetData() const = 0;
    virtual std::string GetFilePath() const = 0;

    // Like GetData(), but for sources that fetch asynchronously (e.g. a
    // worker-thread-backed ScriptDataSource), waits for a fresh fetch to
    // complete (bounded) instead of returning whatever's currently cached.
    // Used only for the Hidden-triggered instant-apply path, where showing
    // stale/blank data on first appearance would be wrong. Synchronous
    // sources (file-backed) are already always fresh, so the default just
    // delegates to GetData().
    virtual std::vector<Record> GetDataBlocking() const { return GetData(); }

    // Lets an asynchronous source apply a self-originated event (e.g. a
    // script calling its own trigger_in()/trigger_out()) without waiting for
    // something else to call GetData() first. Title::Tick calls this
    // unconditionally, every tick, regardless of state — including Hidden,
    // where nothing else ever polls this source — so a script that triggers
    // its own (currently Hidden) title is guaranteed to eventually take
    // effect. No-op by default; synchronous sources have nothing to pump.
    virtual void PumpEvents() const {}

    virtual void SetOwner(Title* owner) { m_owner = owner; }

protected:
    Title* m_owner{nullptr};
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
