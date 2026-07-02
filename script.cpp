// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "script.h"

#include <iostream>
#include <optional>

#include "title.h"

ScriptDataSource::ScriptDataSource(const std::string& path)
    : scriptFilePath(path)
{
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
    L.safe_script_file(path);
    m_getData = L["_get_data"];
    m_onTriggerIn = L["_on_trigger_in"];
    m_onTriggerOut = L["_on_trigger_out"];

    L.set_function("trigger_in", [this](sol::optional<size_t> recordIndex, sol::optional<double> duration) {
        TriggerIn(recordIndex.value_or(0), duration.value_or(-1.0));
    });
    L.set_function("trigger_out", [this] {
        TriggerOut();
    });
}

std::vector<Record> ScriptDataSource::GetData() const
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

void ScriptDataSource::TriggerIn(size_t recordIndex, double duration) {
    m_owner->TriggerIn(recordIndex, duration);
}

void ScriptDataSource::TriggerOut() {
    m_owner->TriggerOut();
}

void ScriptDataSource::SetOwner(Title* owner) {
    if (owner == m_owner) return;
    IDataSource::SetOwner(owner);
    if (!owner) return;

    owner->onTriggerIn.push_back([this](size_t recordIndex, double duration) {
        if (m_onTriggerIn.valid())
            m_onTriggerIn(recordIndex, duration);
    });
    owner->onTriggerOut.push_back([this] {
        if (m_onTriggerOut.valid())
            m_onTriggerOut();
    });
}
