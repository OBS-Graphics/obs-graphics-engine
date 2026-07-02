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
