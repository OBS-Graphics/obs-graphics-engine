# TASK: Lua data-format modules + trigger API trim
> Created: 2026-07-25 | Updated: 2026-07-25

## Goal
Give Lua scripts first-class JSON and XML parsing (they previously got only the raw
`http.get(...).body` string), remove the script→host `trigger_in()` global, and record
findings on how data sources are bound to Titles.

## Plan
- [x] `lua_json.h/cpp` — `json` global (`decode`, `encode`, `null`, `array`) over nlohmann/json
- [x] `lua_xml.h/cpp` — `xml` global (`decode`, `find`, `find_all`) over pugixml
- [x] pugixml fetched via CPM `DOWNLOAD_ONLY`, hand-rolled PIC static target, Lua-gated
- [x] Register both in `ScriptDataSource::WorkerMain` before `safe_script_file`
- [x] Remove `trigger_in` binding + `ScriptDataSource::TriggerIn`; collapse the outbound
      mailbox to `std::atomic<bool> m_pendingOutTrigger`
- [x] Rework `test_script_trigger.cpp` scenarios B and F
- [x] New `tests/test_script_data_parse.cpp`
- [x] Update `CLAUDE.md` / `README.md`
- [x] Data-source investigation (report only, no code change)

## Log
### 2026-07-25
- Configured with Lua on — pugixml fetched:
  `cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DENABLE_LUA_SCRIPTING=ON`
  → `-- CPM: Adding package pugixml@1.15 (v1.15)`, exit 0
- Built everything: `cmake --build build -j$(nproc)` → exit 0, no warnings from engine
  sources (only the pre-existing `liblua.a` `tmpnam` linker warning)
- New json/xml test: `./build/test_script_data_parse` → 46 checks, "All checks passed.",
  exit 0
- Reworked trigger test: `./build/test_script_trigger` → "All checks passed.", exit 0.
  stderr shows `attempt to call a nil value (global 'trigger_in')`, confirming the
  binding is genuinely gone rather than silently no-op
- Untouched http test still green: `./build/test_script_http` → "All checks passed.", exit 0
- No-Lua build path verified: `cmake -B <scratch>/build-nolua -DENABLE_LUA_SCRIPTING=OFF`
  → only nlohmann_json/stb/cpp_zipper added (no pugixml, no lua, no curl), configure exit 0;
  `cmake --build <scratch>/build-nolua` → "Built target engine", exit 0

## Unverified / Pending
- Nothing outstanding for this task.

## Errors & Fixes
| Error | Cause | Fix | Evidence |
|-------|-------|-----|----------|
| First configure added no Lua packages | Existing `build/` cache had `ENABLE_LUA_SCRIPTING:BOOL=OFF` from an earlier run | Reconfigured with `-DENABLE_LUA_SCRIPTING=ON` | `-- CPM: Adding package pugixml@1.15`, exit 0 |

## Current State
All four pieces are implemented and verified by the commands in the Log.

Scripts now get three injected globals — `http` (existing), `json`, and `xml` — registered
in `ScriptDataSource::WorkerMain` after `open_libraries()` and before `safe_script_file()`,
so they are usable at script load time. `json`/`xml` are pure functions with no
process-global state and return `nil, errmsg` instead of raising. `xml.decode` builds plain
Lua tables (`{name, attr, text, children}`), so no pugixml document outlives the call and
XPath is deliberately not exposed.

`trigger_in()` is removed from the Lua API. `trigger_out()`, `_on_trigger_in`, and
`_on_trigger_out` are unchanged. The script→host mailbox is now a single `atomic<bool>`
(`m_pendingOutMutex` and the `TriggerRequest` mailbox use are gone); `TriggerRequest` itself
survives for the inbound host→script job queue.

Data sources: investigation only, no code changed. Findings are in
`/home/diego/.claude-personal/plans/plan-some-adjustments-for-functional-taco.md` — the
headline items are that `Title::dataSource` is a raw non-owning pointer that is never
serialized into `.ogt` (so a loaded Title always comes back unbound), `SetOwner` is called
on every data pull rather than once, and `JsonFileDataSource`/`CsvFileDataSource` are
untested dead code that throw straight out of `Title::Tick` on a missing file.

Next: nothing queued. If the data-source findings are to be acted on later, the natural
first step is deciding whether `Title` should own its `IDataSource` and persist the binding.
