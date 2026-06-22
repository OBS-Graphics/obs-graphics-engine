// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include <sol/sol.hpp>

#include "data-source.h"

class ScriptDataSource : public IDataSource {
public:
    std::string scriptFilePath;

    ScriptDataSource(const std::string& path);
    std::vector<Record> GetData() const override;
    std::string GetFilePath() const override
    {
        return scriptFilePath;
    }

private:
    sol::state L;
    sol::protected_function m_getData;
};
