// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include <chrono>
#include <functional>
#include <string>

namespace sol {
class state;
}

// Blocking, LuaSocket-style `http` Lua global table (get/post/put/patch/
// delete_). Only ever safe to call from a script's own worker thread — see
// ScriptDataSource in script.h/cpp, which is the only intended caller of
// Register().
namespace lua_http {

// Per-request ceiling applied to every http.* call (connect + transfer).
// Deliberately separate from ScriptDataSource's own GetDataBlocking() wait
// bound in script.cpp: a single _get_data() may issue several sequential
// http.* calls, each individually within this budget, that together take
// longer than any one of them.
inline constexpr std::chrono::seconds kRequestTimeout{10};

// Injects the `http` global table into L. Call once per sol::state, after
// open_libraries() and before safe_script_file(), so `http` is available
// anywhere in the script, not just inside _get_data.
void Register(sol::state& L);

// Minimal network-permission-gate extension point — deliberately thin, not a
// real policy layer. Checked once per http.* call, before any socket work.
// Process-global (one gate for every script in the process) and carries no
// caller identity (no script path/title id), so it can only ever express
// "allow everything" or "deny everything" process-wide today. A real
// per-script policy (domain allowlist, explicit opt-in) will need a gate
// that receives that identity — e.g. Register(L, gate) taking a per-script
// gate — which is a replacement of this hook, not an extension of it.
using PermissionGate = std::function<bool(const std::string& method, const std::string& url)>;
void SetPermissionGate(PermissionGate gate);

}  // namespace lua_http
