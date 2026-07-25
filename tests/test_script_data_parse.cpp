// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test (no SDL, no rendering): verifies the `json` and `xml`
// Lua globals a ScriptDataSource injects (lua_json.cpp / lua_xml.cpp).
//
// The assertions themselves live in Lua — a single script runs them all and
// returns the outcomes as a record, which is also the end-to-end proof that a
// script can parse a JSON/XML payload straight into engine records. Keeping
// them in one script means one worker thread and one Lua state for the whole
// suite instead of one per case.

#include "script.h"
#include "script_test_util.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using script_test_util::Check;
using script_test_util::g_failures;
using script_test_util::PollUntil;
using script_test_util::WriteScript;

namespace {

const char* kScript = R"lua(
local checks = {}

local function check(name, cond, detail)
    checks[name] = cond and "ok" or ("FAIL (" .. tostring(detail) .. ")")
end

-- ── json.decode ───────────────────────────────────────────────────────────

local obj = json.decode('{"a":1,"b":"two","c":true,"d":null,"f":1.5}')
check("json.decode returns a table", type(obj) == "table", type(obj))
check("json.decode integer", obj.a == 1, obj.a)
check("json.decode string", obj.b == "two", obj.b)
check("json.decode boolean", obj.c == true, obj.c)
check("json.decode null maps to json.null", obj.d == json.null, tostring(obj.d))
check("json.decode float", obj.f == 1.5, obj.f)

local arr = json.decode('[10,20,30]')
check("json.decode array length", #arr == 3, #arr)
check("json.decode array is 1-based", arr[1] == 10 and arr[3] == 30, tostring(arr[1]))

local nested = json.decode('{"list":[{"k":"v"},{"k":"w"}]}')
check("json.decode nested", nested.list[2].k == "w", tostring(nested.list[2]))

local bad, badErr = json.decode('{oops')
check("json.decode malformed returns nil", bad == nil, tostring(bad))
check("json.decode malformed returns an error string",
      type(badErr) == "string" and #badErr > 0, tostring(badErr))

-- ── json.encode ───────────────────────────────────────────────────────────

check("json.encode array", json.encode({1, 2, 3}) == "[1,2,3]", json.encode({1, 2, 3}))
check("json.encode object", json.encode({a = 1, b = "x"}) == '{"a":1,"b":"x"}',
      json.encode({a = 1, b = "x"}))
check("json.encode empty table is an object", json.encode({}) == "{}", json.encode({}))
check("json.encode json.array{} is an array", json.encode(json.array({})) == "[]",
      json.encode(json.array({})))
check("json.encode json.null", json.encode({x = json.null}) == '{"x":null}',
      json.encode({x = json.null}))
check("json.encode integral float drops the .0", json.encode({n = 1.0}) == '{"n":1}',
      json.encode({n = 1.0}))
check("json.encode keeps a real fraction", json.encode({n = 1.5}) == '{"n":1.5}',
      json.encode({n = 1.5}))
check("json.encode indent adds newlines", json.encode({a = 1}, 2):find("\n") ~= nil,
      json.encode({a = 1}, 2))

local roundTrip = json.decode(json.encode(nested))
check("json.encode/decode round trip", roundTrip.list[1].k == "v", tostring(roundTrip.list[1]))

local cyclic = {}
cyclic.self = cyclic
local cycleOut, cycleErr = json.encode(cyclic)
check("json.encode rejects a cycle", cycleOut == nil, tostring(cycleOut))
check("json.encode cycle returns an error string",
      type(cycleErr) == "string" and #cycleErr > 0, tostring(cycleErr))

local fnOut, fnErr = json.encode({f = print})
check("json.encode rejects an unsupported value", fnOut == nil, tostring(fnOut))
check("json.encode unsupported value returns an error string",
      type(fnErr) == "string" and #fnErr > 0, tostring(fnErr))

-- ── xml.decode ────────────────────────────────────────────────────────────

local simple = xml.decode('<root a="1" b="two">hi</root>')
check("xml.decode returns a table", type(simple) == "table", type(simple))
check("xml.decode node name", simple.name == "root", tostring(simple.name))
check("xml.decode attributes", simple.attr.a == "1" and simple.attr.b == "two",
      tostring(simple.attr.a))
check("xml.decode text", simple.text == "hi", tostring(simple.text))
check("xml.decode children is always present", type(simple.children) == "table",
      type(simple.children))

local doc = xml.decode(
    '<root><!-- ignored --><item id="1">a</item><item id="2">b</item><other/></root>'
)
check("xml.decode skips comments", #doc.children == 3, #doc.children)
check("xml.decode child order", doc.children[1].attr.id == "1" and doc.children[2].attr.id == "2",
      tostring(doc.children[1].attr.id))
check("xml.decode child text", doc.children[2].text == "b", tostring(doc.children[2].text))

local mixed = xml.decode('<root>a<c>b</c></root>')
check("xml.decode text is direct children only", mixed.text == "a", tostring(mixed.text))
check("xml.decode nested text belongs to the child", mixed.children[1].text == "b",
      tostring(mixed.children[1].text))

local cdata = xml.decode('<root><![CDATA[x&y]]></root>')
check("xml.decode CDATA", cdata.text == "x&y", tostring(cdata.text))

local badXml, badXmlErr = xml.decode('<a><b></a>')
check("xml.decode malformed returns nil", badXml == nil, tostring(badXml))
check("xml.decode malformed returns an error string",
      type(badXmlErr) == "string" and #badXmlErr > 0, tostring(badXmlErr))

-- ── xml.find / xml.find_all ───────────────────────────────────────────────

check("xml.find hit", xml.find(doc, "other").name == "other", tostring(xml.find(doc, "other")))
check("xml.find miss returns nil", xml.find(doc, "nope") == nil, tostring(xml.find(doc, "nope")))
check("xml.find returns the first match", xml.find(doc, "item").attr.id == "1",
      tostring(xml.find(doc, "item").attr.id))
check("xml.find_all hit count", #xml.find_all(doc, "item") == 2, #xml.find_all(doc, "item"))
check("xml.find_all miss is an empty table", #xml.find_all(doc, "nope") == 0,
      #xml.find_all(doc, "nope"))

-- ── end to end: parse payloads straight into engine records ───────────────

function _get_data()
    local jsonPayload = json.decode('[{"name":"Ada","role":"Engineer"}]')
    local xmlPayload = xml.decode('<people><person><name>Grace</name></person></people>')
    local person = xml.find(xmlPayload, "person")

    return {
        checks,
        {
            json_name = jsonPayload[1].name,
            json_role = jsonPayload[1].role,
            xml_name  = xml.find(person, "name").text,
        },
    }
end
)lua";

}  // namespace

int main()
{
    std::string path = WriteScript("data_parse", kScript);

    ScriptDataSource ds(path);

    std::vector<Record> recs;
    bool got = PollUntil(
        [&] {
            recs = ds.GetData();
            return recs.size() >= 2;
        },
        std::chrono::seconds(5)
    );

    if (!got) {
        std::fprintf(
            stderr,
            "FAIL: script produced no records (load failed: %s, error: %s)\n",
            ds.LoadFailed() ? "yes" : "no",
            ds.GetLoadError().c_str()
        );
        return 1;
    }

    // Record 0 is the in-Lua check table: every value must be exactly "ok".
    Check(!recs[0].empty(), "script ran its own checks");
    for (const auto& [name, outcome] : recs[0]) {
        std::string label = name + " -> " + outcome;
        Check(outcome == "ok", label.c_str());
    }

    // Record 1 is the end-to-end payload, in the shape Title::UpdateData
    // would map onto element ids.
    const auto& e2e = recs[1];
    Check(e2e.count("json_name") && e2e.at("json_name") == "Ada", "end to end: JSON field became a record value");
    Check(e2e.count("json_role") && e2e.at("json_role") == "Engineer", "end to end: second JSON field");
    Check(e2e.count("xml_name") && e2e.at("xml_name") == "Grace", "end to end: XML field became a record value");

    if (g_failures == 0) {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
