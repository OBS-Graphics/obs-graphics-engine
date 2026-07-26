# TASK: Scene + pull-model data (replace DataPool bindings)
> Created: 2026-07-26 | Updated: 2026-07-26

## Goal
Replace the binding-based `DataPool` (commits 5778192 / 3056aa3 / 7f7d8dd) with a
pull model: `DataPool` becomes a title-blind cache of data sources, `Title` reads it
by `dataSourceId`, and a new `Scene` owns the titles plus their pool and is the world
a Lua script sees.

## Plan
- [x] `uuid.h/cpp` — `GenerateV4()` / `IsV4()`
- [x] `IDataSource`: generated uuid + `GetId`/`SetId`, `TitleRef{id,name}`,
      `SetTitleDirectory`, TitleRef-based `NotifyTriggerIn/Out`, drop `AliveToken`
- [x] Shrink `DataPool`: delete bindings/thunks/titleLock/visibility gating; key by
      source id; add cache `version` + `DataIfChanged`; source-facing relays
- [x] `Title`: `id` = uuid, new `name`, `dataSourceId` + `DataPool*`, restored
      `UpdateData()` / `TriggerIn(recordIndex, duration)`, per-tick version-gated pull
- [x] `.ogt` schema 1.0 → 1.1 + `{1,0}→{1,1}` migration (legacy `"id"` → `"name"`)
- [x] `scene.h/cpp` — `Scene`: AddTitle/RemoveTitle/FindByName/FindById, Tick
      (pool tick → publish directory → dispatch trigger_out → tick titles), Render by zOrder
- [x] `ScriptDataSource`: title handles `{id,name}` in `_on_trigger_in`/`_on_trigger_out`,
      `trigger_out(handle|id|bare)`, new `scene` global (`find_titles`, `titles`)
- [x] Rewrite `tests/test_data_pool.cpp`, add `tests/test_scene.cpp`, update
      `tests/test_script_trigger.cpp`; register both headless tests with ctest
- [x] Update `CLAUDE.md` / `README.md`

## Log
### 2026-07-26
- Engine builds clean with Lua on:
  `cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DENABLE_LUA_SCRIPTING=ON`
  then `cmake --build build -j$(nproc)` → "Built target engine", exit 0, no engine warnings
  (only the pre-existing `liblua.a` `tmpnam` linker warning)
- Full suite green: `ctest --test-dir build --output-on-failure` →
  `100% tests passed, 0 tests failed out of 5` (test_data_pool, test_scene,
  test_script_trigger, test_script_data_parse, test_script_http), total 2.68s
- `./build/test_data_pool` → "All checks passed.", exit 0 (52 checks incl. the
  slow-destructor/concurrent-Tick timing case and version/changed-flag cases)
- `./build/test_scene` → "All checks passed.", exit 0 (45 checks incl. uuid de-dup,
  Visible-only apply, blocking TriggerIn fetch, trigger_out dedup/state guard, zOrder render)
- `./build/test_script_trigger` → "All checks passed.", exit 0 (28 checks incl. the new
  `scene.find_titles("a")` → `trigger_out(t)` end-to-end scenario)
- No-Lua path still builds: `cmake -B <scratch>/build-nolua -DENABLE_LUA_SCRIPTING=OFF
  -DBUILD_TESTS=OFF` → only nlohmann_json/stb/cpp_zipper added (no lua, curl, pugixml),
  configure exit 0; `cmake --build` → "Built target engine", exit 0
- Schema 1.0 → 1.1 round-trip verified with a scratch binary against a hand-built 1.0
  `.ogt` (`{"schema_version":{"major":1,"minor":0},"id":"lower_third",...,"custom_future_key":{...}}`):
  load → `id=91231868-ece9-4f32-a4b7-bb2b35990a25` (fresh uuid), `name=lower_third`,
  `diag_severity=0` (no warning), `extra={"custom_future_key":{"kept":true}}`;
  re-save → `schema_version {'major': 1, 'minor': 1}`, `id` matches the v4 uuid regex,
  `name: lower_third`, metadata and the unknown key both preserved

## Unverified / Pending
- Nothing outstanding for this task. Consumers outside this repo (plugin/editor) have
  NOT been updated — the API changes below are breaking for them.

## Errors & Fixes
| Error | Cause | Fix | Evidence |
|-------|-------|-----|----------|
| `FAIL: ScenarioB: bare Lua trigger_out() drove state to AnimatingOut` | Test artifact: ScenarioB's Title had no elements, so `Scene::Tick` applied the trigger *and* completed the whole out-animation in the same tick (nothing to wait on) — the poll never observed `AnimatingOut`. Verified by temporary stderr traces showing the drain firing correctly with `state=2` (Visible) | Gave that title one short-animation element | `./build/test_script_trigger` → "All checks passed.", exit 0 |

## Current State
Implemented and verified by the commands in the Log.

`DataPool` now owns `IDataSource`s and nothing else: it polls each on a 0.25s cadence,
caches the records, and bumps a per-source `version` only when the fetched records
actually differ. It never includes `title.h`. `Title` stores `dataSourceId` +
`DataPool*` and pulls: `Title::Tick` calls `UpdateData()` every frame while Visible
(an unchanged cache is one integer compare), and `TriggerIn` does a blocking
`DataBlocking` fetch so a title coming from Hidden shows current data. `Scene` owns
the titles plus the pool, publishes the `{id,name}` directory into the sources when it
changes, dispatches script-originated `trigger_out(...)` requests onto titles, and
ticks/renders everything (zOrder ordered, Hidden skipped). Scene persistence is
deliberately the host's job.

Titles carry two identities: `id` (uuid, unique per Scene — `AddTitle` reassigns a
duplicate) and `name` (human, may collide). `.ogt` schema is at 1.1; pre-1.1 files have
their old `"id"` (the name) migrated into `"name"` and get a generated uuid.

Lua sees title handles `{id=, name=}` instead of bind-name strings, gains a `scene`
table (`find_titles(name)`, `titles()`), and `trigger_out` accepts a handle, a uuid
string, or nothing (all titles on that source). `trigger_in` remains absent.

Breaking for out-of-repo consumers: `DataPool::Bind`/`Unbind`/`BoundTitleNames`/
`Keys`/`Data(key)`-by-host-key are gone, `Add` now takes only the source and returns
its id, `Title::TriggerIn` lost its `records` parameter, and `Title::id` no longer
holds the human name (use `Title::name`).

Next: nothing queued.
