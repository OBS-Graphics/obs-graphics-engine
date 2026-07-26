# obs-graphics-engine

Cairo/Pango rendering engine for animated broadcast graphics. A static C++20 library used by both the [obs-graphics](https://github.com/OBS-Graphics/obs-graphics) OBS plugin and the [obs-graphics-editor](https://github.com/OBS-Graphics/obs-graphics-editor) standalone editor.

## Dependencies

### System (must be installed)

| Package | Ubuntu/Debian | Fedora |
|---|---|---|
| Cairo + Pango | `libcairo2-dev libpango1.0-dev` | `cairo-devel pango-devel` |
| minizip | `libminizip-dev` | `minizip-devel` |
| OpenSSL ≥ 3.0 *(only if `ENABLE_LUA_SCRIPTING`)* | `libssl-dev` | `openssl-devel` |
| CMake ≥ 3.16 | `cmake` | `cmake` |

```bash
# Ubuntu / Debian
sudo apt-get install libcairo2-dev libpango1.0-dev libminizip-dev libssl-dev cmake

# Fedora
sudo dnf install cairo-devel pango-devel minizip-devel openssl-devel cmake
```

On Windows, install OpenSSL via [vcpkg](https://github.com/microsoft/vcpkg) (`vcpkg install openssl`) and configure with `-DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake`.

### Fetched automatically by CPM at configure time

| Library | Version | Purpose |
|---|---|---|
| [lua](https://github.com/lua/lua) | 5.4.6 | Scripting runtime |
| [sol2](https://github.com/ThePhD/sol2) | v3.5.0 | C++ Lua bindings |
| [curl/curl](https://github.com/curl/curl) | curl-8_21_0 | HTTP(S) client backing the Lua `http.*` table (statically linked, OpenSSL backend) |
| [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) | v0.49.0 | Local mock HTTP server, **test-only** (not linked into `engine`) |
| [zeux/pugixml](https://github.com/zeux/pugixml) | v1.15 | XML parser backing the Lua `xml.*` table (statically linked) |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.12.0 | JSON parsing/serialization, and the Lua `json.*` table |
| [nothings/stb](https://github.com/nothings/stb) | master | Image loading (stb_image) |
| [yhirose/cpp-zipper](https://github.com/yhirose/cpp-zipper) | master | ZIP I/O for `.ogt` files (wraps system minizip) |

No manual setup needed — CPM downloads and configures all of these on first configure. `curl`, `pugixml`, and `cpp-httplib` are only fetched when `ENABLE_LUA_SCRIPTING` is on (the last additionally requires `BUILD_TESTS`).

## Building standalone

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Produces `build/libengine.a`.

## Using as a submodule

```bash
git submodule add <url> engine
```

In your `CMakeLists.txt`:

```cmake
add_subdirectory(engine)
target_link_libraries(your-target PRIVATE engine)
```

Headers are then available as:

```cpp
#include "engine/title.h"
#include "engine/types.hpp"
// etc.
```

## Core types

| Type | Description |
|---|---|
| `Scene` | Owns the Titles a host has open plus their `DataPool`; ticks and renders them — the per-collection/profile runtime unit. |
| `Title` | Self-contained presentation unit — width/height, element tree, state machine, load/save `.ogt`. |
| `IElement` | Base class: id, parent/child tree. |
| `Spatial` | Adds bounds, rotation, shear, world-position tracking. |
| `VisualElement` | Adds fill/stroke, opacity, shadow, mask, in/out animations, data-change animations. |
| `RectangleElement` / `TextElement` / `ImageElement` / `QrElement` | Concrete element types. |
| `Paint` | Solid, Linear, Radial, or Image fill — normalized 0–1 gradient coordinates. |
| `AnimationDef` | Per-element animation: type, easing, duration, delay. |
| `IDataSource` | Interface for a JSON/CSV/Lua source of records; carries a generated uuid, knows nothing about any Title. |
| `DataPool` | Owns every `IDataSource`, polls each on a 0.25s cadence, and caches (and versions) what it got — see below. |
| `ScriptDataSource` | Lua-scripted data source (via sol2) — reacts to a Title's trigger state, and gets `scene`/`http`/`json`/`xml` globals. |

## .ogt file format

Each `Title` is saved as a standard ZIP archive with the `.ogt` extension:

```
my_title.ogt  (ZIP)
├── title.json          — title definition
├── thumb.png           — optional preview thumbnail
└── ASSETS/
    ├── logo.png
    └── bg.jpg
```

Paths in `title.json` starting with `@` refer to bundled assets (`"@logo.png"` → `ASSETS/logo.png`). Any other string is treated as a plain filesystem path and is not bundled.

```cpp
Title t = Title::Load("lower_third.ogt");
t.TriggerIn();
// ... in render loop:
t.Tick(dt);
t.Render(cairo_ctx);
```

## Title state machine

```
Hidden ──TriggerIn()──▶ AnimatingIn ──▶ Visible ──TriggerOut()──▶ AnimatingOut ──▶ Hidden
```

While `Visible`, `Tick()` advances per-element data-change animations and pulls fresh records from the pool (see **Scene, data sources & DataPool** below). Pass a `duration` to `TriggerIn()` to auto-`TriggerOut()` after that many seconds Visible (a "timed title"); omit it (or pass `-1`) to stay Visible until explicitly triggered out.

Every `Title` also carries two identities: `id`, a generated uuid that is unique within a `Scene` and is what a Lua script targets, and `name`, the human-readable name an operator sees. Both round-trip through `title.json`.

`Title::onTriggerIn` / `onTriggerOut` are lists of callbacks fired whenever `TriggerIn()`/`TriggerOut()` runs, from any origin — a host UI, the `duration` timeout, or a script's own `trigger_out()` — so a host application can react to a title's state changing regardless of what caused it.

## Scene, data sources & DataPool

A `Scene` owns the Titles a host has open and the `DataPool` behind them. `DataPool` owns every `IDataSource` and does exactly one job: poll each source on a 0.25s cadence and cache what it got. It knows nothing about Titles — a `Title` stores the id of the source it reads and pulls that cache itself:

```cpp
Scene scene;
auto sourceId = scene.Pool().Add(std::make_unique<JsonFileDataSource>("scores.json"));

Title* title = scene.AddTitle(std::make_unique<Title>(Title::Load("scoreboard.ogt")));
title->dataSourceId = sourceId;

title->TriggerIn();  // blocking fresh fetch, then animates in
// ... in render loop, once per frame:
scene.Tick(dt);      // refresh sources, dispatch script triggers, tick every title
scene.Render(cairo_ctx);
```

Pointing a second `Title` at the same `dataSourceId` shares the same underlying source — several titles can read one feed with no per-title fetching and no ownership handoff. Sources generate their own uuid at construction (`IDataSource::GetId()`), which is what `DataPool::Add` keys by and returns; `SetId()` lets a host restore an id it persisted. The title↔source mapping itself is *not* saved in `.ogt` — that's the host's to own per collection/profile.

Each cache carries a version that only moves when the fetched records actually differ, so a `Title` can poll every frame and still animate only on real change (`DataPool::DataIfChanged`). Showing a `Hidden` title is the one blocking path: `TriggerIn()` fetches through `DataPool::DataBlocking` (bounded) and applies the result instantly before animating in, because stale or blank data on first appearance would be worse than a brief wait.

`IDataSource` implementations (`JsonFileDataSource`, `CsvFileDataSource`, `ScriptDataSource`) supply per-element content, looked up by element id and applied via `SetContent`/`SetContentInstant`. `ScriptDataSource` runs a Lua script that must define `_get_data()` (returns a table of records), and may optionally define `_on_trigger_in(title, recordIndex, duration)` / `_on_trigger_out(title)` to react whenever a title reading it is triggered — where `title` is a `{ id = ..., name = ... }` handle. A script can call `trigger_out(title)` with such a handle to hide one specific title, or the bare `trigger_out()` to hide every title reading that source; there is deliberately no `trigger_in` counterpart, since whether a title is on-screen at all is the host's call.

**Threading** — a `Scene` and its Titles are single-threaded: `Tick`/`Render`/`AddTitle`/`TriggerIn`/`TriggerOut` all belong to the host's render thread. `DataPool`'s own mutex covers only source registration and caches, so a host UI thread may still `Add`/`Remove` sources; `DataBlocking()` releases that mutex before the (potentially slow) fetch, and sources are destroyed after it is released, so neither ever stalls `Tick()`.

Scripts get four global tables injected by the engine:

| Table | Functions |
|---|---|
| `scene` | `find_titles(name)` → list of `{id, name}` handles, `titles()` → all of them |
| `http` | `get`, `post`, `put`, `patch`, `delete_` — blocking HTTP(S) via libcurl |
| `json` | `decode`, `encode`, `null`, `array` — via nlohmann/json |
| `xml` | `decode`, `find`, `find_all` — via pugixml |

```lua
for _, t in ipairs(scene.find_titles("ticker")) do
    trigger_out(t)
end
```

```lua
function _get_data()
    local res = http.get("https://api.example.com/scores")
    if not res.ok or res.status ~= 200 then return {} end

    local data = json.decode(res.body)
    if not data then return {} end

    return { { home = data.home.name, away = data.away.name } }
end
```

`json.decode`/`json.encode` and `xml.decode` return `nil, errmsg` on failure rather than raising, so a malformed payload is a branch, not a script error. `xml.decode` produces plain Lua tables (`{ name, attr, text, children }`), so a node stays valid after the call returns.
