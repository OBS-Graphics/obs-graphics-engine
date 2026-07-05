// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "lua_http.h"

#include <sol/sol.hpp>

#include <curl/curl.h>

#include <map>
#include <mutex>
#include <string>

namespace lua_http {
namespace {

using ResponseHeaders = std::multimap<std::string, std::string>;

std::mutex g_gateMutex;
PermissionGate g_gate;  // default-constructed = empty = allow all

bool IsRequestAllowed(const std::string& method, const std::string& url)
{
    std::lock_guard<std::mutex> lock(g_gateMutex);
    return !g_gate || g_gate(method, url);
}

std::once_flag g_curlInitFlag;
void EnsureCurlGlobalInit()
{
    std::call_once(g_curlInitFlag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t WriteBodyCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

// Fires once per header line of every response in the redirect chain
// (including intermediate hops) — clear on each new status line so only the
// final response's headers survive.
size_t WriteHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
    auto* headers = static_cast<ResponseHeaders*>(userdata);
    std::string line(buffer, size * nitems);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();

    if (line.rfind("HTTP/", 0) == 0) {
        headers->clear();
        return size * nitems;
    }

    auto colon = line.find(':');
    if (colon == std::string::npos) return size * nitems;

    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    size_t start = value.find_first_not_of(' ');
    value = (start == std::string::npos) ? std::string() : value.substr(start);
    headers->emplace(std::move(key), std::move(value));
    return size * nitems;
}

sol::table BuildHttpResponseTable(
    sol::state_view lua,
    bool ok,
    int status,
    const std::string& body,
    const ResponseHeaders& headers,
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

    EnsureCurlGlobalInit();
    CURL* curl = curl_easy_init();
    if (!curl)
        return BuildHttpResponseTable(lua, false, 0, "", {}, "curl_easy_init failed");

    std::string responseBody;
    ResponseHeaders responseHeaders;
    struct curl_slist* headerList = nullptr;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    // Set unconditionally (including for GET) so one code path handles every
    // verb without branching, mirroring the previous single-function
    // generic-dispatch shape.
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, verb.c_str());

    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
    }

    if (headers) {
        for (auto& [k, v] : *headers) {
            std::string line = k.as<std::string>() + ": " + v.as<std::string>();
            headerList = curl_slist_append(headerList, line.c_str());
        }
        if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteBodyCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &WriteHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(kRequestTimeout.count()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(kRequestTimeout.count()));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    // Required: DoRequest always runs on a ScriptDataSource worker thread,
    // never the main thread, and libcurl's default signal-based timeouts are
    // documented as unsafe outside a single-threaded/main-thread caller.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);

    bool ok = (res == CURLE_OK);
    long statusCode = 0;
    std::string errorMsg;
    if (ok)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    else
        errorMsg = curl_easy_strerror(res);

    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    return BuildHttpResponseTable(lua, ok, static_cast<int>(statusCode), responseBody, responseHeaders, errorMsg);
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

    bindNoBody("get", "GET");
    bindWithBody("post", "POST");
    bindWithBody("put", "PUT");
    bindWithBody("patch", "PATCH");
    bindNoBody("delete_", "DELETE");
}

void SetPermissionGate(PermissionGate gate)
{
    std::lock_guard<std::mutex> lock(g_gateMutex);
    g_gate = std::move(gate);
}

}  // namespace lua_http
