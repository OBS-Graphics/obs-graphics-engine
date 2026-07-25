// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

namespace sol {
class state;
}

// `xml` Lua global table (decode/find/find_all), backed by pugixml. The
// counterpart to lua_json for scripts whose upstream speaks XML rather than
// JSON.
//
// Pure functions over their arguments — no process-global state, so like
// lua_json (and unlike lua_http) this needs no mutex and no permission gate.
namespace lua_xml {

// Maximum element nesting depth decode() will walk before giving up with an
// error. Mirrors lua_json::kMaxDepth.
inline constexpr int kMaxDepth = 200;

// Injects the `xml` global table into L. Call once per sol::state, after
// open_libraries() and before safe_script_file(), so `xml` is available
// anywhere in the script, not just inside _get_data.
//
// Bound API:
//   xml.decode(str)          -> root node table | nil, errmsg
//   xml.find(node, name)     -> first child element with that name, or nil
//   xml.find_all(node, name) -> array of matching child elements (may be empty)
//
// decode() converts eagerly into plain Lua tables, so there is no pugixml
// document lifetime for a script to manage and a node can outlive the call:
//
//   { name     = "item",
//     attr     = { id = "1" },  -- always present, may be empty
//     text     = "hello",       -- direct text/CDATA children, concatenated
//     children = { <node>, .. } -- element children, document order }
//
// XPath is deliberately not exposed: it would require keeping the
// pugi::xml_document alive behind userdata, which is exactly the lifetime
// problem the eager-table shape avoids. find/find_all are thin scans over
// `node.children` so scripts read cleanly without nested ipairs loops.
void Register(sol::state& L);

}  // namespace lua_xml
