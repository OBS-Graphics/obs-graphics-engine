// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "lua_json.h"

#include <sol/sol.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace lua_json {
namespace {

// Identity-compared sentinel behind `json.null`. Its address is the only
// thing that matters; the byte itself is never read.
char g_nullSentinel = 0;
void* NullSentinel()
{
    return &g_nullSentinel;
}

bool IsNullSentinel(const sol::object& v)
{
    return v.get_type() == sol::type::lightuserdata && v.as<void*>() == NullSentinel();
}

// ── JSON -> Lua ───────────────────────────────────────────────────────────

sol::object ToLua(sol::state_view lua, const nlohmann::json& j, int depth, std::string& err);

sol::table ObjectToLua(sol::state_view lua, const nlohmann::json& j, int depth, std::string& err)
{
    sol::table t = lua.create_table();
    for (auto it = j.begin(); it != j.end(); ++it) {
        t[it.key()] = ToLua(lua, it.value(), depth + 1, err);
        if (!err.empty()) break;
    }
    return t;
}

sol::table ArrayToLua(sol::state_view lua, const nlohmann::json& j, int depth, std::string& err)
{
    sol::table t = lua.create_table(static_cast<int>(j.size()), 0);
    int i = 1;
    for (const auto& item : j) {
        t[i++] = ToLua(lua, item, depth + 1, err);
        if (!err.empty()) break;
    }
    return t;
}

sol::object ToLua(sol::state_view lua, const nlohmann::json& j, int depth, std::string& err)
{
    if (depth > kMaxDepth) {
        err = "JSON nesting deeper than " + std::to_string(kMaxDepth) + " levels";
        return sol::lua_nil;
    }

    switch (j.type()) {
        case nlohmann::json::value_t::object:
            return ObjectToLua(lua, j, depth, err);
        case nlohmann::json::value_t::array:
            return ArrayToLua(lua, j, depth, err);
        case nlohmann::json::value_t::string:
            return sol::make_object(lua, j.get<std::string>());
        case nlohmann::json::value_t::boolean:
            return sol::make_object(lua, j.get<bool>());
        case nlohmann::json::value_t::number_integer:
            return sol::make_object(lua, j.get<int64_t>());
        case nlohmann::json::value_t::number_unsigned:
            return sol::make_object(lua, static_cast<int64_t>(j.get<uint64_t>()));
        case nlohmann::json::value_t::number_float:
            return sol::make_object(lua, j.get<double>());
        case nlohmann::json::value_t::null:
        default:
            return sol::make_object(lua, sol::lightuserdata_value(NullSentinel()));
    }
}

// ── Lua -> JSON ───────────────────────────────────────────────────────────

// Explicit `__jsontype = "array"|"object"` metatable field, which is also how
// json.array() tags a table. Overrides the shape heuristic below.
sol::optional<std::string> JsonTypeTag(const sol::table& t)
{
    sol::optional<sol::table> mt = t[sol::metatable_key];
    if (!mt) return sol::nullopt;
    return (*mt)["__jsontype"];
}

// True when the table's keys are exactly the integers 1..n (n > 0), i.e. a
// dense Lua sequence. An empty table is *not* an array by this rule — it
// encodes as {} unless explicitly tagged, since an empty JSON object is the
// safer default for a record-shaped payload.
bool LooksLikeArray(const sol::table& t, size_t& outLength)
{
    size_t count = 0;
    size_t maxIndex = 0;
    for (const auto& [k, v] : t) {
        (void)v;
        if (k.get_type() != sol::type::number) return false;
        double d = k.as<double>();
        if (d != std::trunc(d) || d < 1.0) return false;
        maxIndex = std::max(maxIndex, static_cast<size_t>(d));
        ++count;
    }
    outLength = count;
    return count > 0 && maxIndex == count;
}

std::string KeyToString(const sol::object& k, bool& ok)
{
    ok = true;
    if (k.get_type() == sol::type::string) return k.as<std::string>();
    if (k.get_type() == sol::type::number) {
        double d = k.as<double>();
        if (d == std::trunc(d)) return std::to_string(static_cast<int64_t>(d));
        return std::to_string(d);
    }
    ok = false;
    return {};
}

bool ToJson(
    const sol::object& v,
    nlohmann::json& out,
    int depth,
    std::vector<const void*>& openTables,
    std::string& err
);

bool TableToJson(
    const sol::table& t,
    nlohmann::json& out,
    int depth,
    std::vector<const void*>& openTables,
    std::string& err
)
{
    const void* id = t.pointer();
    for (const void* open : openTables) {
        if (open == id) {
            err = "cannot encode a table that contains itself";
            return false;
        }
    }
    openTables.push_back(id);

    size_t length = 0;
    bool looksLikeArray = LooksLikeArray(t, length);
    auto tag = JsonTypeTag(t);
    bool asArray = tag ? (*tag == "array") : looksLikeArray;
    // An explicitly tagged table that isn't a dense sequence (e.g. an empty
    // one from json.array{}) gets its length from the # operator instead.
    if (asArray && !looksLikeArray) length = t.size();

    bool ok = true;
    if (asArray) {
        out = nlohmann::json::array();
        for (size_t i = 1; i <= length; ++i) {
            nlohmann::json item;
            if (!ToJson(t[i], item, depth + 1, openTables, err)) {
                ok = false;
                break;
            }
            out.push_back(std::move(item));
        }
    } else {
        out = nlohmann::json::object();
        for (const auto& [k, val] : t) {
            bool keyOk = false;
            std::string key = KeyToString(k, keyOk);
            if (!keyOk) {
                err = "table key of unsupported type (only strings and numbers can be object keys)";
                ok = false;
                break;
            }
            nlohmann::json item;
            if (!ToJson(val, item, depth + 1, openTables, err)) {
                ok = false;
                break;
            }
            out[key] = std::move(item);
        }
    }

    openTables.pop_back();
    return ok;
}

bool ToJson(
    const sol::object& v,
    nlohmann::json& out,
    int depth,
    std::vector<const void*>& openTables,
    std::string& err
)
{
    if (depth > kMaxDepth) {
        err = "value nested deeper than " + std::to_string(kMaxDepth) + " levels";
        return false;
    }

    if (IsNullSentinel(v)) {
        out = nullptr;
        return true;
    }

    switch (v.get_type()) {
        case sol::type::nil:
        case sol::type::none:
            out = nullptr;
            return true;
        case sol::type::boolean:
            out = v.as<bool>();
            return true;
        case sol::type::string:
            out = v.as<std::string>();
            return true;
        case sol::type::number: {
            // Lua 5.4 has a real integer subtype, but sol's integral check is
            // permissive enough to accept 1.5 as an int — decide from the
            // value instead, so 1.0 encodes as 1 and 1.5 stays 1.5.
            double d = v.as<double>();
            if (!std::isfinite(d)) {
                err = "cannot encode a non-finite number (nan/inf) as JSON";
                return false;
            }
            if (d == std::trunc(d) && std::abs(d) <= 9007199254740992.0)
                out = static_cast<int64_t>(d);
            else
                out = d;
            return true;
        }
        case sol::type::table:
            return TableToJson(v.as<sol::table>(), out, depth, openTables, err);
        default:
            err = "cannot encode a value of type '" + std::string(sol::type_name(v.lua_state(), v.get_type())) + "'";
            return false;
    }
}

// ── bindings ──────────────────────────────────────────────────────────────

sol::variadic_results Fail(sol::state_view lua, const std::string& message)
{
    sol::variadic_results r;
    r.push_back(sol::make_object(lua, sol::lua_nil));
    r.push_back(sol::make_object(lua, message));
    return r;
}

sol::variadic_results Decode(const std::string& text, sol::this_state ts)
{
    sol::state_view lua(ts);

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        return Fail(lua, e.what());
    }

    std::string err;
    sol::object value = ToLua(lua, parsed, 0, err);
    if (!err.empty()) return Fail(lua, err);

    sol::variadic_results r;
    r.push_back(value);
    return r;
}

sol::variadic_results Encode(const sol::object& value, sol::optional<int> indent, sol::this_state ts)
{
    sol::state_view lua(ts);

    nlohmann::json j;
    std::string err;
    std::vector<const void*> openTables;
    if (!ToJson(value, j, 0, openTables, err)) return Fail(lua, err);

    sol::variadic_results r;
    try {
        r.push_back(sol::make_object(lua, j.dump(indent.value_or(-1))));
    } catch (const std::exception& e) {
        return Fail(lua, e.what());
    }
    return r;
}

sol::table Array(sol::table t, sol::this_state ts)
{
    sol::state_view lua(ts);
    sol::optional<sol::table> mt = t[sol::metatable_key];
    if (!mt) {
        sol::table created = lua.create_table();
        t[sol::metatable_key] = created;
        mt = created;
    }
    (*mt)["__jsontype"] = "array";
    return t;
}

}  // namespace

void Register(sol::state& L)
{
    sol::table json = L.create_named_table("json");
    json.set_function("decode", &Decode);
    json.set_function("encode", &Encode);
    json.set_function("array", &Array);
    json["null"] = sol::lightuserdata_value(NullSentinel());
}

}  // namespace lua_json
