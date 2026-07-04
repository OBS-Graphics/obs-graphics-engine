// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "lua_http.h"

#include <sol/sol.hpp>

#include <ixwebsocket/IXHttpClient.h>

#include <mutex>
#include <string>

namespace lua_http {
namespace {

std::mutex g_gateMutex;
PermissionGate g_gate;  // default-constructed = empty = allow all

bool IsRequestAllowed(const std::string& method, const std::string& url)
{
    std::lock_guard<std::mutex> lock(g_gateMutex);
    return !g_gate || g_gate(method, url);
}

sol::table BuildHttpResponseTable(
    sol::state_view lua,
    bool ok,
    int status,
    const std::string& body,
    const ix::WebSocketHttpHeaders& headers,
    const std::string& errorMsg
)
{
    sol::table t = lua.create_table();
    t["ok"] = ok;
    t["status"] = status;
    t["body"] = body;
    t["error_msg"] = errorMsg;

    sol::table h = lua.create_table();
    for (auto& [k, v] : headers)
        h[k] = v;
    t["headers"] = h;

    return t;
}

sol::table DoRequest(
    const std::string& verb,
    const std::string& url,
    const sol::optional<std::string>& body,
    const sol::optional<sol::table>& headers,
    sol::state_view lua
)
{
    if (!IsRequestAllowed(verb, url))
        return BuildHttpResponseTable(lua, false, 0, "", {}, "network access denied by policy");

    ix::HttpClient client;
    auto args = client.createRequest(url, verb);
    args->connectTimeout = static_cast<int>(kRequestTimeout.count());
    args->transferTimeout = static_cast<int>(kRequestTimeout.count());

    if (headers) {
        for (auto& [k, v] : *headers)
            args->extraHeaders[k.as<std::string>()] = v.as<std::string>();
    }

    auto response = client.request(url, verb, body.value_or(std::string()), args);

    bool ok = response->errorCode == ix::HttpErrorCode::Ok;
    return BuildHttpResponseTable(lua, ok, response->statusCode, response->body, response->headers, response->errorMsg);
}

}  // namespace

void Register(sol::state& L)
{
    sol::table http = L.create_named_table("http");

    // get/delete_ and post/put/patch are each identical apart from the verb
    // and whether a body argument is accepted — bind through two generic
    // shapes instead of five near-duplicate lambdas.
    auto bindNoBody = [&http](const char* name, std::string verb) {
        http.set_function(name, [verb](std::string url, sol::optional<sol::table> headers, sol::this_state ts) {
            return DoRequest(verb, url, sol::optional<std::string>{}, headers, sol::state_view(ts));
        });
    };
    auto bindWithBody = [&http](const char* name, std::string verb) {
        http.set_function(
            name,
            [verb](std::string url, sol::optional<std::string> body, sol::optional<sol::table> headers,
                   sol::this_state ts) { return DoRequest(verb, url, body, headers, sol::state_view(ts)); }
        );
    };

    bindNoBody("get", ix::HttpClient::kGet);
    bindWithBody("post", ix::HttpClient::kPost);
    bindWithBody("put", ix::HttpClient::kPut);
    bindWithBody("patch", ix::HttpClient::kPatch);
    bindNoBody("delete_", ix::HttpClient::kDelete);
}

void SetPermissionGate(PermissionGate gate)
{
    std::lock_guard<std::mutex> lock(g_gateMutex);
    g_gate = std::move(gate);
}

}  // namespace lua_http
