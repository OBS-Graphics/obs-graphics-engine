// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test (no SDL, no rendering): exercises the `http` Lua
// table (lua_http.h/cpp) end to end through ScriptDataSource's worker
// thread, against a local in-process HTTP server (cpp-httplib's Server,
// test-only dependency) so nothing depends on the public internet. Covers:
// a successful GET, a non-2xx response, a request to an unreachable host,
// and header round-tripping.

#include "script.h"
#include "script_test_util.h"

#include <httplib.h>

#include <chrono>
#include <string>
#include <thread>

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

// Minimal local HTTP server (backed by cpp-httplib's own Server, so no
// per-platform raw-socket code is needed here) with a few canned routes.
struct TestHttpServer {
    httplib::Server server;
    int port = 0;
    std::thread thread;

    TestHttpServer()
    {
        server.set_pre_routing_handler([](const httplib::Request& request, httplib::Response& response) {
            const std::string& path = request.path;

            if (path == "/ok") {
                response.status = 200;
                response.set_content("hello", "text/plain");
            } else if (path == "/missing") {
                response.status = 404;
                response.set_content("nope", "text/plain");
            } else if (path == "/echo-headers") {
                std::string echoed;
                if (auto it = request.headers.find("X-Test-Header"); it != request.headers.end())
                    echoed = it->second;
                response.status = 200;
                response.set_header("X-Echoed", echoed);
                response.set_content(echoed, "text/plain");
            } else {
                response.status = 400;
            }
            return httplib::Server::HandlerResponse::Handled;
        });

        // Bind happens synchronously here, so the socket is already in the
        // kernel accept backlog before listen_after_bind()'s accept loop
        // even starts on the background thread below.
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
    }

    ~TestHttpServer()
    {
        server.stop();
        if (thread.joinable()) thread.join();
    }

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
    // no dependency on any real network. Bind a throwaway server to learn a
    // free port, then properly stop it so the kernel actually releases the
    // socket — Server's destructor alone does NOT close svr_sock_ unless the
    // listen loop was actually started (bind-then-destroy-without-listening
    // leaves the port bound+listening forever, which turns "connection
    // refused" into a silent hang instead).
    int deadPort;
    {
        httplib::Server probe;
        deadPort = probe.bind_to_any_port("127.0.0.1");
        std::thread probeThread([&probe] { probe.listen_after_bind(); });
        probe.wait_until_ready();
        probe.stop();
        probeThread.join();
    }

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
