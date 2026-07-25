// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

namespace sol {
class state;
}

// `json` Lua global table (decode/encode/null/array), backed by nlohmann/json.
// Exists so a script doesn't have to hand-roll a parser over the raw string
// `http.get(...).body` hands it back.
//
// Pure functions over their arguments — no process-global state, so unlike
// lua_http this needs no mutex and no permission gate, and Register() is safe
// to call on any number of sol::states.
namespace lua_json {

// Maximum nesting depth the Lua <-> nlohmann converters will walk before
// giving up with an error. Bounds our own recursion only; see the note on
// decode() below about the parser's own depth.
inline constexpr int kMaxDepth = 200;

// Injects the `json` global table into L. Call once per sol::state, after
// open_libraries() and before safe_script_file(), so `json` is available
// anywhere in the script, not just inside _get_data.
//
// Bound API:
//   json.decode(str)            -> value        | nil, errmsg
//   json.encode(value[, indent])-> string       | nil, errmsg
//   json.null                   -> sentinel, so a JSON null survives a
//                                  decode -> encode round trip
//   json.array(t)               -> t, tagged so it encodes as [] even empty
//
// JSON -> Lua: object -> string-keyed table, array -> 1-based table,
// string/number/boolean -> native, null -> json.null.
// Lua -> JSON: a table whose keys are exactly 1..n -> array, otherwise
// object; an empty table -> {} unless tagged by json.array. An explicit
// `__jsontype = "array"|"object"` metatable field overrides the heuristic.
//
// Known limitation: kMaxDepth bounds our converters, but nlohmann's own
// parser recursion is unbounded, so a pathologically deep document can still
// exhaust the stack inside decode(). Same exposure as every other nlohmann
// use in the engine.
void Register(sol::state& L);

}  // namespace lua_json
