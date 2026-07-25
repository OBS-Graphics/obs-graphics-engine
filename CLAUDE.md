# CLAUDE.md

Rendering engine for obs-graphics. Static C++20 library. No Qt — renders via Cairo/Pango only.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

sol2, cpp-zipper, and nlohmann/json are fetched automatically by CPM at configure time. cpp-zipper wraps system minizip (`libminizip-dev` must be installed).

sol2/Lua, libcurl, and pugixml are only fetched and built when `ENABLE_LUA_SCRIPTING` (default `ON`) is enabled. Pass `-DENABLE_LUA_SCRIPTING=OFF` to skip them entirely — e.g. for a consumer (like the editor) that never uses `ScriptDataSource`.

## Source files

All sources live at repo root (no `src/` subdirectory):

- `title.h/cpp` — `Title` struct: state machine, render, load/save `.ogt`
- `element.h/cpp` — `IElement` base class (parent/child tree management)
- `spatial.h/cpp` — `Spatial : IElement` — adds bounds, rotation, shear, world-position tracking
- `visual_element.h/cpp` — `VisualElement : Spatial` — common rendering (shadow, clip, opacity group, children, data-change animation); pure virtual `RenderContent`
- `element_rectangle.h/cpp` — `RectangleElement : VisualElement`
- `element_text.h/cpp` — `TextElement : VisualElement`; also owns all text/font enums
- `element_image.h/cpp` — `ImageElement : VisualElement`
- `element_qr.h/cpp` — `QrElement : VisualElement`
- `render_util.h/cpp` — `namespace render`: `RoundRect`, `LoadImageSurface`, `RenderDropShadow`, `RenderDropShadowFromSurface` (box-blur shadow pipeline; both accept an optional `Transform`)
- `animation.h/cpp` — `AnimationDef`, `AnimatedTransform`, easing functions
- `types.hpp` — `Paint`, `Point`, `Size`, `Rectangle`, `Transform`, `ScaleMode` — core shared types
- `data-source.h/cpp` — `IDataSource`, JSON/CSV data source implementations
- `data-pool.h/cpp` — `DataPool`: owns every `IDataSource`, polls each once per tick, and pushes cached records into every `Title` bound to it — see the `DataPool` entry below
- `script.h/cpp` — `ScriptDataSource` (Lua via sol2); compiled only when `ENABLE_LUA_SCRIPTING=ON` (default)
- `lua_http.h/cpp` — `http` Lua global table (libcurl); Lua-only, same CMake gate as `script.cpp`
- `lua_json.h/cpp` — `json` Lua global table (nlohmann/json); Lua-only, same gate
- `lua_xml.h/cpp` — `xml` Lua global table (pugixml); Lua-only, same gate
- `qr.hpp` — single-header QR encoder (header-only, `QR_IMPLEMENTATION` defined in `qr.cpp`)
- `stb.cpp` — single translation unit that provides `stb_image` implementation (`STB_IMAGE_IMPLEMENTATION`)

## Element class hierarchy

```
IElement          (element.h)     — id, parent/child tree, virtual Render/ApplyClipping
  └─ Spatial      (spatial.h)     — bounds, rotation, shear, GetGlobalPosition
       └─ VisualElement (visual_element.h) — fill/stroke, opacity, shadow, mask, animations
            ├─ RectangleElement   (element_rectangle.h)
            ├─ TextElement        (element_text.h)
            ├─ ImageElement       (element_image.h)
            └─ QrElement          (element_qr.h)
```

`Title::elements` is `std::vector<std::unique_ptr<IElement>>`. `elements[0]` is always the auto-created root (`IElement` with id `"__root"`). All other entries are `VisualElement` subclasses.

## Key types

**`IElement`** — Base interface. Manages the parent/child tree via `AddChild`/`RemoveChild`/`SetParent`. `AddChild(child)` calls `child->SetParent(this)`; `RemoveChild(child)` calls `child->SetParent(nullptr)`. Internal list manipulation uses `AddChildDirect`/`RemoveChildDirect` to avoid recursion.

**`Spatial`** — Adds `m_bounds` (local position + size), `m_rotation`, `m_shearX/Y`. `GetTransform()` returns a `Transform` struct bundling rotation + shear. Overrides `SetParent` to adjust `m_bounds.x/y` so the element's **world position is preserved** when reparented: saves `GetGlobalPosition()` before changing parent, then subtracts the new parent's world position. When loading from JSON (where x/y are local), `title.cpp` temporarily converts to world coords before calling `AddChild` so SetParent's subtraction is a net no-op.

**`VisualElement`** — All renderable properties: `fill`, `stroke`, `strokeWidth`, `cornerRadius[4]`, `opacity`, `mask`, `shadow{}`, `inAnimation`/`outAnimation`, `dataInAnimation`/`dataOutAnimation`, `zOrder`, `fitToChildren`. Provides `Render()` (pipeline: shadow → wipe clip → opacity group → mask clip → `GetTransform().Apply()` → `ApplyClipping` → scale → content → children). `SetContent(value)` is **final** — it is a no-op if the value hasn't changed, otherwise drives the data-change animation state machine and calls `ApplyContent(value)` at the right moment. Subclasses override `ApplyContent` instead. Private helpers `ComposeDataAnimation`, `RenderShadow`, `RenderChildren` keep `Render` concise. Subclasses may override `CreateShadowSurface(w, h) → cairo_surface_t*` to supply a custom A8 shadow mask (nullptr = default rounded-rect shadow).

**`Paint`** — Fill or stroke. `type` = Solid / Linear / Radial / Image. Params layout:
- Solid: `params[0..3]` = r,g,b,a
- Linear: `params[0..3]` = x0,y0,x1,y1 (normalized 0–1, scaled by element bounds at render)
- Radial: `params[0..2]` = focus cx,cy,r; `params[3..5]` = main cx,cy,r (normalized by element width)
- Image: loaded via `Paint::Image(path)` using `cairo_image_surface_create_from_png`

**`Transform`** (`types.hpp`) — Bundles `rotation` (degrees), `shearX`, `shearY`. `Apply(ctx, cx, cy)` applies shear-then-rotation to a Cairo context centred at `(cx, cy)` in the current user space; no-op when `IsIdentity()`. `Spatial::GetTransform()` returns one from the element's own fields.

**`namespace render`** (`render_util.h/cpp`) — Stateless utilities:
- `RoundRect(ctx, x, y, w, h, r[4])` — rounded-rect path
- `LoadImageSurface(path)` — stb_image → premultiplied ARGB32 Cairo surface
- `RenderDropShadow(ctx, sx, sy, sw, sh, blur, r, g, b, a, cornerR, transform={})` — 3× separable box-blur (≈Gaussian) of a rounded-rect shape on an A8 surface, composited as a coloured shadow via `cairo_mask_surface()`. O(pixels) regardless of blur radius. When `transform` is non-identity the A8 surface is sized to contain the transformed shape and the shear/rotation is applied inside the A8 draw step.
- `RenderDropShadowFromSurface(ctx, srcA8, destX, destY, blur, r, g, b, a, transform={})` — same blur pipeline but takes a pre-rendered A8 surface as the shadow shape. When `transform` is identity uses a fast `memcpy` path; otherwise applies the transform when copying into the padded A8. Used by `TextElement` to cast text-shaped shadows.

**`Title`** — Standalone presentation unit. Holds `width`/`height`, `metadata` (arbitrary JSON), an elements vector (root at [0], VisualElements at [1..]), and a state machine: `Hidden → AnimatingIn → Visible → AnimatingOut → Hidden`. `Tick(dt)` advances the animation timer and, while Visible, calls `TickData(dt)` on each VisualElement to drive per-element data-change animations. `Render(cr)` draws direct children of root sorted by `zOrder`. Loads/saves as `.ogt` (zip archive, see below).

`Title` holds no data-source pointer and never fetches anything itself — it is a pure consumer of whatever records a caller hands it (see `DataPool` below). `TriggerIn(recordIndex, duration, records)` / `TriggerOut()` drive the state machine and are the single choke point for every trigger origin — a direct host call, the `duration` timeout below, or a script's `trigger_out()` (see Lua scripting). If `records` is non-empty, `TriggerIn` applies it instantly (`SetContentInstant`) before the `AnimatingIn` transition starts — the caller (typically `DataPool::DataBlocking`) is responsible for fetching fresh data first when triggering in from `Hidden`, where showing stale/blank data on first appearance would be wrong. `UpdateData(records, instant)` is the lower-level apply step both `TriggerIn` and `DataPool::Tick` funnel through: it indexes `records[dataRecordIndex % records.size()]` and applies each field to the matching element by id (`SetContentInstant` if `instant`, else the animated `SetContent`), silently ignoring ids with no matching element. Both `TriggerIn`/`TriggerOut` fire every subscriber in `onTriggerIn`/`onTriggerOut` (`std::vector<std::function<...>>` — multiple listeners, e.g. a host UI and a `DataPool` binding, can coexist without clobbering each other).

`duration` (seconds, default `-1.0` = no auto-hide) is passed to `TriggerIn` and, while `Visible`, `Tick` auto-fires `TriggerOut()` once that much Visible-time has elapsed — a "timed title." The periodic 0.25s data refresh that used to live in `Title::Tick` now lives in `DataPool::Tick`, gated on `TitleState::Visible` there instead.

**`AnimationDef` / `AnimatedTransform`** — In/out animation per element. `animation::EvaluateAnimation(def, t, isOut, width, height)` returns an `AnimatedTransform` (offset, scale, opacity, clip rect). Out animations are true reversals. Per-element data-change animations are stored in `dataInAnimation`/`dataOutAnimation` and driven by `VisualElement::TickData`.

**`IDataSource`** (`data-source.h`) — Interface for a source of `Record`s (`Record = unordered_map<string,string>`). Pure virtual `GetData() -> std::vector<Record>` and `GetFilePath()`. A source knows nothing about any `Title` — that mapping belongs entirely to `DataPool` (below), which is what lets several Titles share one instance. `PumpEvents()` is a per-tick housekeeping no-op hook; `DrainOutTriggerRequests()` returns bind names a script asked to hide via `trigger_out(...)` since the last drain (empty-string sentinel = "every title bound to this source"); `NotifyTriggerIn`/`NotifyTriggerOut(bindName, ...)` are called by `DataPool` whenever a bound Title's `onTriggerIn`/`onTriggerOut` fires, so `ScriptDataSource` can relay into `_on_trigger_in`/`_on_trigger_out`. All three are no-ops by default — only `ScriptDataSource` overrides them.

**`DataPool`** (`data-pool.h`) — Owns every `IDataSource` a host registers (`Add(key, source)`/`Remove(key)`), polls each once per tick, and pushes freshly-fetched records into every `Title` bound to it (`Bind(title, key, bindName, titleLock)`/`Unbind(title)`). This is what lets several Titles share one data source — e.g. a weather feed backing both a corner ticker and a full-screen graphic — without the old single-owner `SetOwner` ping-pong (a `Title` binding/re-binding a source on every poll used to reassign a single owning pointer and re-append duplicate trigger callbacks each time). `bindName` is the identity a Lua script sees and can target via `trigger_out(bindName)`/`_on_trigger_in(bindName, ...)`/`_on_trigger_out(bindName)` — deliberately **not** `Title::id`, which comes straight from the loaded `.ogt` file and can collide (two open instances of the same file) or be empty. `Data(key)` reads the last cache (never blocks); `DataBlocking(key)` forces a fresh fetch and is the replacement for the old `TriggerIn`-from-`Hidden`-blocks behavior — the caller fetches via this *before* calling `Title::TriggerIn` with the result. `Tick(dt)` runs, in order: (a) pump + drain every source's `trigger_out(...)` requests and map them back to bound Titles via `TriggerOut()`; (b) refresh each source's cache on the fixed 0.25s cadence (reusing the same prev-slot/curr-slot technique `Title::Tick` used to run per-title), gated on at least one bound Title being `Visible`; (c) push refreshed records into every `Visible` binding via `UpdateData(records, /*instant=*/false)`.

**Locking discipline** — Lock order is strictly `DataPool`'s own mutex → a bound Title's lock (when the host supplies one via `Bind`'s `titleLock`), never the reverse. `DataBlocking()` is the one path that must not nest them: it copies out the raw `IDataSource*` while holding the pool mutex, releases it, then calls the (potentially slow) `GetDataBlocking()` — so a slow fetch never blocks `Tick()`/`Add()`/`Remove()`/`Bind()` on another thread.

## .ogt file format

Each Title is saved as a `.ogt` file — a ZIP archive with this layout:

```
my_title.ogt  (ZIP)
├── title.json          — title definition
├── thumb.png           — optional preview thumbnail
└── ASSETS/
    ├── logo.png
    └── bg.jpg
```

**Asset path convention in title.json** — paths starting with `@` refer to bundled assets:
- `"@logo.png"` → `ASSETS/logo.png` inside the zip
- Any other string → absolute or relative path on the local filesystem (not bundled)

On `Title::Load`: bundled assets are extracted to a system temp directory for the life of the Title object. On `Title::~Title`: the temp directory is removed.

On `Title::Save`: all referenced asset files (whether `@`-prefixed or local paths) are copied into `ASSETS/` and paths are rewritten as `@basename` in the JSON.

## title.json schema

```json
{
  "id": "lower_third",
  "width": 1920,
  "height": 1080,
  "metadata": { "editor_zoom": 1.5, "custom_key": "value" },
  "elements": [
    { "type": "rectangle", "id": "bg", "x": 0, "y": 0, "w": 800, "h": 100 },
    { "type": "image", "id": "logo", "image_path": "@logo.png" },
    { "type": "text", "id": "name", "parent": "bg", "x": 10, "y": 10 }
  ]
}
```

The root element (`"__root"`) is **not** serialized. Elements without a `parent` field are automatically parented to root on load. The `metadata` object is omitted when empty.

## JSON schema (element fields)

| Field | Type | Notes |
|---|---|---|
| `type` | string | `"rectangle"`, `"text"`, `"image"`, `"qr_code"` |
| `x`, `y`, `w`, `h` | float | Bounds — **local (parent-relative)** when `parent` is set |
| `image_path` | string | Image file path or `@name` for bundled asset |
| `scale_mode` | string | `"stretch"` (default), `"contain"`, `"cover"`, `"fit_width"`, `"fit_height"`, `"none"`, `"tile"` |
| `fill` / `stroke` | array, object, or string | Array = solid RGBA; object = linear/radial/image gradient; string = image path |
| `text_align_x` | string | `"left"`, `"center"`, `"right"`, `"justify"` |
| `text_align_y` | string | `"top"`, `"middle"`, `"bottom"` |
| `auto_scale` | bool | Shrink font to fit bounds |
| `ellipsize` | string | `"none"`, `"start"`, `"middle"`, `"end"` |
| `wrap` | string | `"word"`, `"char"`, `"word_char"` |
| `text_transform` | string | `"none"`, `"capitalize"`, `"uppercase"`, `"lowercase"` |
| `line_spacing` | float | Extra pixels between lines (default 0); maps to `pango_layout_set_spacing` |
| `letter_spacing` | float | Pixels between characters (default 0); maps to `pango_attr_letter_spacing_new` |
| `shear_x` | float | Horizontal shear factor (default 0) |
| `shear_y` | float | Vertical shear factor (default 0) |
| `shadow` | object | Drop shadow: `enabled` (bool), `offset_x`, `offset_y`, `blur` (floats), `color` ([r,g,b,a] array) |
| `mask` | string | id of another element to use as a clip mask |
| `parent` | string | id of parent element; omit to parent directly to root |
| `anim_in` / `anim_out` | object | In/out animation def: `type`, `easing`, `duration`, `delay` |
| `data_anim_in` / `data_anim_out` | object | Data-change animation (same structure as anim_in/out) |
| `metadata` *(title-level)* | object | Arbitrary key-value store for editor/consumer settings; omitted when empty |

## Schema versioning & forward compatibility

`.ogt` files carry a schema version so a plugin/editor can safely open a file written
by a *different* version of the engine. The policy lives in `title.cpp`.

**Version constants** — `kOgtSchemaMajor` / `kOgtSchemaMinor` (currently **1.0**).
- `Save` writes `"schema_version": { "major": M, "minor": m }` and also a legacy
  `"version": M` int for readers that predate the major/minor split.
- `Load` reads `schema_version` when present, else falls back to the legacy `version`
  int (via migration, below), else treats the file as the current version.

**Bump policy**
- **Minor bump** — additive, backward-compatible change: a new optional field with a
  safe default, a new element type an older reader can skip. Older engines still load
  the file; they just don't understand the new field (but *preserve* it — see below).
- **Major bump** — a breaking change: a field's meaning/shape changes, a required field
  is added, or old readers would mis-render. Older engines flag the file as newer-major.

**Unknown-field preservation (the core forward-compat guarantee)** — the engine no
longer discards JSON keys it doesn't recognize. Unknown keys on an element are captured
into `VisualElement::extra` and unknown top-level keys into `Title::extra`, then merged
back on `Save` (typed/known keys win over `extra`). This means an *older* editor can open,
edit, and re-save a file written by a *newer* plugin **without stripping** the newer
plugin's fields. This is what makes minor-version cross-compatibility real rather than
nominal.

**Read policy** — `ClassifySchema(fileMajor, fileMinor)` returns a `SchemaCompat`:
| Result | Condition | Behavior |
|---|---|---|
| `Ok` | same major, same/older minor | load normally |
| `OlderMigrate` | older file | run migration, then load |
| `NewerMinor` | same major, newer minor | load; unknown fields preserved; **info** diagnostic |
| `NewerMajor` | newer major | load-degraded (not rejected): unknown fields preserved, new features ignored; **warning** diagnostic |

Newer-major is intentionally **load-degraded, not reject** — combined with unknown-field
preservation, an older editor opening a far-newer file still shows what it can and
round-trips the rest untouched.

**Migration hooks** — `MigrateTitleJson(json&, fromMajor, fromMinor)` runs in `Load`
between parse and field extraction, driven by an ordered migration registry (each step
transforms the JSON in place from one version to the next). The only current step is the
`{0,0}` legacy/absent-version normalization → current shape. To add a migration: append a
step keyed by the source version that walks the JSON (recursing into `elements` and their
children for element-field changes) and rewrites it to the next version's shape.

**Load diagnostic** — instead of printing to `stderr`, `Load` records a
`Title::LoadDiagnostic { Severity{None,Info,Warning}, message }`, readable via
`Title::GetLoadDiagnostic()`. The editor surfaces it as a non-blocking notice when a file
was created with a newer schema version.

## Lua scripting (`ScriptDataSource`)

Built only when the CMake option `ENABLE_LUA_SCRIPTING` (default `ON`) is enabled; when disabled, `script.h/cpp` are excluded from the `engine` target and Lua/sol2 are never fetched. `engine` publicly defines `ENGINE_HAS_LUA_SCRIPTING` only when the feature is built, so a consumer can guard any conditional `#include "engine/script.h"` with `#ifdef ENGINE_HAS_LUA_SCRIPTING`.

`ScriptDataSource` (`script.h/cpp`) runs a Lua 5.4 script (via sol2) as an `IDataSource`. It knows nothing about any `Title` — only about `DataPool`'s `bindName`, called `titleId` from the script's point of view since that's the only "which title" identity a script ever sees. Contract, all optional except `_get_data`:

| Lua global | Direction | Called |
|---|---|---|
| `_get_data()` | script → engine | Every `GetData()` pull (instant fetch via `DataPool::DataBlocking`, or the 0.25s Visible-gated poll in `DataPool::Tick`). Must return a table of record-tables (string/number/bool values, converted to strings). |
| `_on_trigger_in(titleId, recordIndex, duration)` / `_on_trigger_out(titleId)` | engine → script | Whenever a bound `Title` is triggered in/out, from **any** origin (host call, `duration` timeout, or this script's own `trigger_out`) — wired up in `DataPool::Bind` as a subscriber on that `Title`'s `onTriggerIn`/`onTriggerOut`, exactly like a host UI listener would be. `titleId` is the `bindName` that binding was made with, so a script bound to several titles can tell them apart. |
| `trigger_out(titleId)` | script → engine | Bound into the Lua globals on the worker thread, **after** `safe_script_file` — so it is callable from `_get_data`/`_on_trigger_*`, but *not* at script load time. Queues `titleId` (or the empty-string sentinel for the bare `trigger_out()` form, meaning "every title bound to this source") in a mutex-guarded list that `DataPool::Tick` drains via `DrainOutTriggerRequests()` and applies as `TriggerOut()` on the matching bound `Title`(s), on the host thread. |

There is deliberately **no `trigger_in`** counterpart: whether a title is on-screen at all is the host's call, so a script asking to be *shown* isn't something the engine can honour meaningfully. Scripts still learn about every show through `_on_trigger_in`.

These are two independent, one-directional channels — `_on_trigger_in`/`_on_trigger_out` do not imply `trigger_out` and vice versa. See `tests/test_script_trigger.cpp` for exercised examples of each, including two Titles bound to one `ScriptDataSource`.

### Injected Lua globals

Three tables are registered on the `sol::state` in `ScriptDataSource::WorkerMain`, after `open_libraries()` and **before** `safe_script_file()`, so they are usable anywhere in the script rather than only inside `_get_data`:

| Table | File | Functions |
|---|---|---|
| `http` | `lua_http.h/cpp` | `get`, `post`, `put`, `patch`, `delete_` — blocking libcurl requests; each returns `{ok, status, body, error_msg, headers}`. Has a process-global `SetPermissionGate` hook. |
| `json` | `lua_json.h/cpp` | `decode`, `encode`, `null`, `array` — over nlohmann/json |
| `xml` | `lua_xml.h/cpp` | `decode`, `find`, `find_all` — over pugixml |

`json`/`xml` are pure functions over their arguments (no process-global state, no mutex, no permission gate) and return `nil, errmsg` on failure rather than raising, so a malformed payload is a branch in the script, not a script error.

- `json.decode` maps object → string-keyed table, array → 1-based table, `null` → the `json.null` lightuserdata sentinel. `json.encode` treats a table keyed exactly `1..n` as an array and anything else as an object; `json.array(t)` tags `t` (via a `__jsontype` metatable field) so an empty table still encodes as `[]`. Cycles and non-encodable values are rejected with an error string. Integral floats encode without a `.0`; non-finite numbers are an error.
- `xml.decode` converts eagerly into plain Lua tables — `{ name, attr, text, children }`, where `text` is the concatenated *direct* text/CDATA children only — so nothing holds a `pugi::xml_document` alive and a node survives the call. XPath is intentionally not exposed for that reason; `find`/`find_all` are thin scans over `node.children`.
- Both cap recursion at `kMaxDepth` (200). Note nlohmann's own parser depth is unbounded, so a pathologically deep JSON document can still exhaust the stack inside `json.decode`.

`tests/test_script_data_parse.cpp` covers both tables; the assertions themselves live in Lua and come back as a record, which doubles as the end-to-end proof that a script can parse a payload straight into engine records.

## Conventions

- Gradient coordinates in `Paint` are normalized 0–1 and scaled by element bounds at Cairo render time.
- Mask clipping offset is computed via `GetGlobalPosition()` on both elements — never via raw bounds — so clipping is correct when either element has a parent.
- Image element always clips to its own bounds before painting, so Cover/FitWidth/FitHeight/None scale modes cannot bleed outside the element rect.
- Drop shadow always uses the full element bounds (including glyph outlines from `CreateShadowSurface`). During wipe animations a ctx clip limits shadow visibility to the currently-revealed area; the transform (shear + rotation) is applied inside the shadow pipeline, not by the caller.
- `TextElement` casts a text-shaped shadow (actual glyph outlines blurred) via `CreateShadowSurface`.
- Data-change animations compose on top of graphic in/out animations: opacities multiply, offsets add, scales multiply. A wipe data animation overrides the graphic wipe clip.
- `SOL_NO_THREAD_LOCAL` is defined (only when `ENABLE_LUA_SCRIPTING=ON`) to avoid Lua thread-local overhead.
- `-fPIC` is required because this static lib is linked into a shared library (OBS module).

## Include path for consumers

The CMakeLists exports `${CMAKE_CURRENT_SOURCE_DIR}/..` as the PUBLIC include directory. When this repo is a submodule at `engine/`, consumers include as:

```cpp
#include "engine/title.h"
#include "engine/types.hpp"
// etc.
```
