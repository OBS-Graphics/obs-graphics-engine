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
| `Title` | Self-contained presentation unit — width/height, element tree, state machine, load/save `.ogt`. |
| `IElement` | Base class: id, parent/child tree. |
| `Spatial` | Adds bounds, rotation, shear, world-position tracking. |
| `VisualElement` | Adds fill/stroke, opacity, shadow, mask, in/out animations, data-change animations. |
| `RectangleElement` / `TextElement` / `ImageElement` / `QrElement` | Concrete element types. |
| `Paint` | Solid, Linear, Radial, or Image fill — normalized 0–1 gradient coordinates. |
| `AnimationDef` | Per-element animation: type, easing, duration, delay. |
| `IDataSource` | Interface for a JSON/CSV/Lua source of records; knows nothing about any Title. |
| `DataPool` | Owns every `IDataSource`, polls each once per tick, and pushes records into every Title bound to it — see below. |
| `ScriptDataSource` | Lua-scripted data source (via sol2) — reacts to a bound Title's trigger state, and gets `http`/`json`/`xml` globals. |

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

While `Visible`, `Tick()` advances per-element data-change animations. `Title` no longer polls any data source itself — see **Data sources & DataPool** below for how records reach a `Title` on a per-frame basis. Pass a `duration` to `TriggerIn()` to auto-`TriggerOut()` after that many seconds Visible (a "timed title"); omit it (or pass `-1`) to stay Visible until explicitly triggered out.

`Title::onTriggerIn` / `onTriggerOut` are lists of callbacks fired whenever `TriggerIn()`/`TriggerOut()` runs, from any origin — a host UI, the `duration` timeout, or a script's own `trigger_out()` — so a host application can react to a title's state changing regardless of what caused it.

## Data sources & DataPool

`Title` holds no data-source pointer and never fetches anything itself — it only ever applies records a caller hands it, via `TriggerIn(recordIndex, duration, records)` (instant apply, e.g. from `Hidden`) or `UpdateData(records, instant)` (the shared apply step, e.g. from a periodic poll). `DataPool` owns every `IDataSource`, polls each once per tick, and pushes freshly-fetched records into every `Title` bound to it:

```cpp
DataPool pool;
pool.Add("scoreboard", std::make_unique<JsonFileDataSource>("scores.json"));
pool.Bind(&title, "scoreboard", /*bindName=*/"main");
// ... in render loop, once per frame:
pool.Tick(dt);       // refreshes the source (Visible-gated, 0.25s cadence) and delivers to bound Titles
title.Tick(dt);
title.Render(cairo_ctx);
```

Binding a second `Title` to the same key (`pool.Bind(&title2, "scoreboard", "aux")`) shares the same underlying source — this is the whole point of `DataPool`: no single-owner `SetOwner` ping-pong when more than one Title wants the same feed. `bindName` (`"main"`/`"aux"` above) is the identity a Lua script sees as `titleId` and can target with `trigger_out(titleId)`.

`IDataSource` implementations (`JsonFileDataSource`, `CsvFileDataSource`, `ScriptDataSource`) supply per-element content, looked up by element id and applied via `SetContent`/`SetContentInstant`. `ScriptDataSource` runs a Lua script that must define `_get_data()` (returns a table of records), and may optionally define `_on_trigger_in(titleId, recordIndex, duration)` / `_on_trigger_out(titleId)` to react whenever a bound Title is triggered. A script can also call `trigger_out(titleId)` to hide one specific bound title, or the bare `trigger_out()` to hide every title bound to that source; there is deliberately no `trigger_in` counterpart, since whether a title is on-screen at all is the host's call.

Showing a `Hidden` title with fresh data is the caller's responsibility: fetch via `pool.DataBlocking(key)` (blocks for a real fetch, bounded) and pass the result to `TriggerIn`:

```cpp
auto records = pool.DataBlocking("scoreboard");
title.TriggerIn(/*recordIndex=*/0, /*duration=*/-1.0, records);
```

**Locking** — `DataPool`'s own mutex is always locked before a bound Title's lock (if the host passes one to `Bind`), never the reverse. `DataBlocking()` is the exception that proves the rule: it releases the pool mutex before the (potentially slow) fetch, so it never stalls `Tick()`/`Add()`/`Remove()`/`Bind()` on another thread.

Scripts get three global tables injected by the engine:

| Table | Functions |
|---|---|
| `http` | `get`, `post`, `put`, `patch`, `delete_` — blocking HTTP(S) via libcurl |
| `json` | `decode`, `encode`, `null`, `array` — via nlohmann/json |
| `xml` | `decode`, `find`, `find_all` — via pugixml |

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
