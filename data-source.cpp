// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "data-source.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

std::vector<Record> JsonFileDataSource::GetData() const
{
    std::vector<Record> records;

    std::ifstream f(filePath);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open data source file: " + filePath);
    }

    json j;
    f >> j;

    if (!j.is_array()) {
        throw std::runtime_error("Expected JSON array in data source file: " + filePath);
    }

    for (const auto& item : j) {
        if (!item.is_object())
            continue;

        Record record;
        for (auto it = item.begin(); it != item.end(); ++it) {
            if (it.value().is_string()) {
                record[it.key()] = it.value().get<std::string>();
            } else {
                record[it.key()] = it.value().dump();
            }
        }
        records.push_back(std::move(record));
    }

    return records;
}

std::vector<Record> CsvFileDataSource::GetData() const
{
    std::vector<Record> records;

    std::ifstream f(filePath);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open data source file: " + filePath);
    }

    std::string line;
    std::vector<std::string> headers;

    // Read header line
    if (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) {
            headers.push_back(cell);
        }
    } else {
        throw std::runtime_error("CSV file is empty: " + filePath);
    }

    // Read data lines
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string cell;
        Record record;
        size_t idx = 0;
        while (std::getline(ss, cell, ',')) {
            if (idx < headers.size()) {
                record[headers[idx]] = cell;
            }
            idx++;
        }
        records.push_back(std::move(record));
    }

    return records;
}

ManualDataSource::ManualDataSource(Table table) : m_table(std::move(table)) {}

std::vector<Record> ManualDataSource::GetData() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<Record> records;
    records.reserve(m_table.rows.size());

    for (const auto& row : m_table.rows) {
        Record record;
        for (size_t c = 0; c < m_table.columns.size(); ++c) {
            const std::string& name = m_table.columns[c].name;
            // No element id can be empty, so a column with no name cannot
            // match one — skip it rather than storing a key nothing reads.
            if (name.empty()) continue;
            // Record is an unordered_map, so two columns sharing a name
            // collapse into one entry — last column wins, silently.
            record[name] = c < row.size() ? row[c] : "";
        }
        records.push_back(std::move(record));
    }

    return records;
}

std::string ManualDataSource::GetDisplayName() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_table.name;
}

ManualDataSource::Table ManualDataSource::GetTable() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_table;
}

void ManualDataSource::SetTable(Table table)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_table = std::move(table);
}
