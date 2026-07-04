// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test (no SDL, no rendering): exercises the `http` Lua
// table (lua_http.h/cpp) end to end through ScriptDataSource's worker
// thread, against a local in-process HTTP server (ix::HttpServer) so nothing
// depends on the public internet. Covers: a successful GET, a non-2xx
// response, a request to an unreachable host, and header round-tripping.

#include "script.h"
#include "script_test_util.h"

#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXHttpServer.h>

#include <chrono>
#include <string>

using script_test_util::Check;
using script_test_util::g_failures;
using script_test_util::PollUntil;
using script_test_util::WriteScript;

namespace {

std::vector<Record> PollForRecords(ScriptDataSource& ds)
{
    std::vector<Record> result;
    // A more generous timeout than the 2s default: these fetches involve a
    // real (if local) socket round trip on top of the worker thread's own
    // startup latency.
    PollUntil(
        [&] {
            result = ds.GetData();
            return !result.empty();
        },
        std::chrono::seconds(5)
    );
    return result;
}

// Minimal local HTTP server (backed by IXWebSocket's own HttpServer, so no
// per-platform raw-socket code is needed here) with a few canned routes.
struct TestHttpServer {
    int port;
    ix::HttpServer server;

    TestHttpServer() : port(ix::getFreePort()), server(port, "127.0.0.1")
    {
        server.setOnConnectionCallback(
            [](ix::HttpRequestPtr request, std::shared_ptr<ix::ConnectionState>) -> ix::HttpResponsePtr {
                const std::string& uri = request->uri;

                if (uri == "/ok") {
                    return std::make_shared<ix::HttpResponse>(
                        200, "OK", ix::HttpErrorCode::Ok, ix::WebSocketHttpHeaders{}, std::string("hello")
                    );
                }
                if (uri == "/missing") {
                    return std::make_shared<ix::HttpResponse>(
                        404, "Not Found", ix::HttpErrorCode::Ok, ix::WebSocketHttpHeaders{}, std::string("nope")
                    );
                }
                if (uri == "/echo-headers") {
                    std::string echoed;
                    auto it = request->headers.find("X-Test-Header");
                    if (it != request->headers.end()) echoed = it->second;

                    ix::WebSocketHttpHeaders respHeaders;
                    respHeaders["X-Echoed"] = echoed;
                    return std::make_shared<ix::HttpResponse>(
                        200, "OK", ix::HttpErrorCode::Ok, respHeaders, echoed
                    );
                }
                return std::make_shared<ix::HttpResponse>(
                    400, "Bad Request", ix::HttpErrorCode::Ok, ix::WebSocketHttpHeaders{}, std::string()
                );
            }
        );

        auto result = server.listen();
        if (!result.first) {
            std::fprintf(stderr, "TestHttpServer failed to listen on port %d: %s\n", port, result.second.c_str());
        }
        server.start();
    }

    ~TestHttpServer() { server.stop(); }

    std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(port); }
};

void TestGetOk(const std::string& baseUrl)
{
    std::string script = "function _get_data()\n"
                          "  local resp = http.get(\"" +
        baseUrl +
        "/ok\")\n"
        "  return { { ok = tostring(resp.ok), status = tostring(resp.status), body = resp.body } }\n"
        "end\n";

    ScriptDataSource ds(WriteScript("get_ok", script));
    auto recs = PollForRecords(ds);
    Check(!recs.empty(), "HTTP GET /ok: got a record");
    if (!recs.empty()) {
        Check(recs[0]["ok"] == "true", "HTTP GET /ok: ok == true");
        Check(recs[0]["status"] == "200", "HTTP GET /ok: status == 200");
        Check(recs[0]["body"] == "hello", "HTTP GET /ok: body round-tripped");
    }
}

void TestGetNon2xx(const std::string& baseUrl)
{
    std::string script = "function _get_data()\n"
                          "  local resp = http.get(\"" +
        baseUrl +
        "/missing\")\n"
        "  return { { ok = tostring(resp.ok), status = tostring(resp.status) } }\n"
        "end\n";

    ScriptDataSource ds(WriteScript("get_missing", script));
    auto recs = PollForRecords(ds);
    Check(!recs.empty(), "HTTP GET /missing: got a record");
    if (!recs.empty()) {
        Check(recs[0]["ok"] == "true", "HTTP GET /missing: ok == true (transport succeeded)");
        Check(recs[0]["status"] == "404", "HTTP GET /missing: status == 404");
    }
}

void TestUnreachableHost()
{
    // A port nothing is listening on: fast, deterministic connection refusal,
    // no dependency on any real network.
    int deadPort = ix::getFreePort();

    std::string script = "function _get_data()\n"
                          "  local resp = http.get(\"http://127.0.0.1:" +
        std::to_string(deadPort) +
        "/\")\n"
        "  return { { ok = tostring(resp.ok), status = tostring(resp.status),\n"
        "             has_err = tostring(resp.error_msg ~= \"\") } }\n"
        "end\n";

    ScriptDataSource ds(WriteScript("unreachable", script));
    auto recs = PollForRecords(ds);
    Check(!recs.empty(), "HTTP GET unreachable host: got a record");
    if (!recs.empty()) {
        Check(recs[0]["ok"] == "false", "HTTP GET unreachable host: ok == false");
        Check(recs[0]["status"] == "0", "HTTP GET unreachable host: status == 0");
        Check(recs[0]["has_err"] == "true", "HTTP GET unreachable host: error_msg populated");
    }
}

void TestHeaderRoundTrip(const std::string& baseUrl)
{
    std::string script = "function _get_data()\n"
                          "  local resp = http.get(\"" +
        baseUrl +
        "/echo-headers\", { [\"X-Test-Header\"] = \"abc123\" })\n"
        "  return { { echoed_header = resp.headers[\"X-Echoed\"] or \"\", body = resp.body } }\n"
        "end\n";

    ScriptDataSource ds(WriteScript("headers", script));
    auto recs = PollForRecords(ds);
    Check(!recs.empty(), "HTTP header round-trip: got a record");
    if (!recs.empty()) {
        Check(recs[0]["echoed_header"] == "abc123", "HTTP header round-trip: header echoed back in response header");
        Check(recs[0]["body"] == "abc123", "HTTP header round-trip: header echoed back in response body");
    }
}

}  // namespace

int main()
{
    TestHttpServer server;
    std::string baseUrl = server.BaseUrl();

    TestGetOk(baseUrl);
    TestGetNon2xx(baseUrl);
    TestUnreachableHost();
    TestHeaderRoundTrip(baseUrl);

    if (g_failures == 0) {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
