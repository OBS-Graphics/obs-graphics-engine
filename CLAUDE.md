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

- `scene.h/cpp` — `Scene`: owns the open `Title`s plus their `DataPool`; ticks/renders them and is the world a Lua script sees — see the `Scene` entry below
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
- `data-source.h/cpp` — `IDataSource` (+ `TitleRef`), JSON/CSV/manual data source implementations
- `data-pool.h/cpp` — `DataPool`: owns every `IDataSource`, polls each on a fixed cadence, and caches/versions the result — see the `DataPool` entry below
- `uuid.h/cpp` — `uuid::GenerateV4()` / `uuid::IsV4()`; the identity of both `IDataSource` and `Title`
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

**`Title`** — Standalone presentation unit. Holds `id` (a generated uuid — see below), `name` (human-readable), `width`/`height`, `metadata` (arbitrary JSON), an elements vector (root at [0], VisualElements at [1..]), and a state machine: `Hidden → AnimatingIn → Visible → AnimatingOut → Hidden`. `Tick(dt)` advances the animation timer and, while Visible, pulls fresh data (below) and calls `TickData(dt)` on each VisualElement to drive per-element data-change animations. `Render(cr)` draws direct children of root sorted by `zOrder`. Loads/saves as `.ogt` (zip archive, see below).

**Move semantics own the asset directory** — `Title` is move-only (copy is deleted). The move constructor and move assignment operator are user-defined, not `= default`: each Title's extracted-asset temp directory (see ".ogt file format" below) is deleted by `~Title`, so a defaulted move — which can leave the moved-from directory path as a copy rather than clearing it — would let the moved-from Title's destructor delete the directory the moved-to Title now depends on. The hand-written versions move every field and use `std::exchange` to clear the source's directory path (move assignment also deletes its *own* directory first, since it's about to lose the only pointer to it). `Title::Load` returns by value into `std::make_unique<Title>(Title::Load(...))`, so this move happens on every load.

**Two identities** — `id` is a `uuid::GenerateV4()` value, unique within a `Scene` (`Scene::AddTitle` reassigns a duplicate, which happens when the same `.ogt` is opened twice since ids round-trip through the file) and is what a Lua script targets. `name` is the operator-facing name, free to collide — which is why `Scene::FindByName` returns a list. Pre-1.1 `.ogt` files stored the *name* in the `"id"` key; the schema migration moves it (see Schema versioning).

**Data** — a `Title` reads at most one data source, named by `dataSourceId` (an `IDataSource::GetId()`) and read out of `dataPool` (set by `Scene::AddTitle`). It pulls; nothing pushes into it:
- `UpdateData()` asks `DataPool::DataIfChanged` and applies the records with the animated `SetContent` **only** when that source's cache version moved. `Title::Tick` calls it every frame while `Visible` — an unchanged cache costs one integer compare, which is why there is no per-title poll timer anymore.
- `TriggerIn(recordIndex, duration)` fetches through `DataPool::DataBlocking` (bounded, but it *can* block) and applies the result instantly (`SetContentInstant`) before the `AnimatingIn` transition — showing stale or blank data on first appearance would be worse.
- `TriggerIn(recordIndex, duration, records)` is the same transition against records the caller **already** fetched, doing no fetch of its own. It exists for a host whose `Scene` is serialized by a lock its render thread also needs (the OBS plugin's `g_scene_mutex`): such a host pulls with `DataPool::DataBlocking` off that thread — `DataBlocking` takes no `Scene` lock — and hands the result here, instead of holding the lock for the seconds a network-backed `ScriptDataSource` takes. Empty `records` means "the source returned nothing", not "go and fetch", which is why it's a separate overload rather than a defaulted argument. It syncs `m_lastDataVersion` to the pool's current version, so the first `Visible` tick doesn't read the caller's own fetch as a change and re-animate content already on screen. Both overloads share one `EnterAnimatingIn` tail, so the state transition, subscribers, and `NotifyTriggerIn` relay cannot drift apart.
- `UpdateData(records, instant)` is the lower-level apply step both funnel through: it indexes `records[dataRecordIndex % records.size()]` and applies each field to the matching element by id, silently ignoring ids with no matching element.
- `TriggerIn`/`TriggerOut` fire every subscriber in `onTriggerIn`/`onTriggerOut` (`std::vector<std::function<...>>` — several host listeners can coexist) and relay into the source via `DataPool::NotifyTriggerIn`/`NotifyTriggerOut`, so a script hears about every show/hide regardless of origin.

`duration` (seconds, default `-1.0` = no auto-hide) is passed to `TriggerIn` and, while `Visible`, `Tick` auto-fires `TriggerOut()` once that much Visible-time has elapsed — a "timed title."

**`AnimationDef` / `AnimatedTransform`** — In/out animation per element. `animation::EvaluateAnimation(def, t, isOut, width, height)` returns an `AnimatedTransform` (offset, scale, opacity, clip rect). Out animations are true reversals. Per-element data-change animations are stored in `dataInAnimation`/`dataOutAnimation` and driven by `VisualElement::TickData`.

**`IDataSource`** (`data-source.h`) — Interface for a source of `Record`s (`Record = unordered_map<string,string>`). Carries a uuid generated at construction (`GetId()`/`SetId()` — the setter is for restoring a host-persisted id), which is what `DataPool` keys by and what a `Title` stores in `dataSourceId`. Pure virtual `GetData() -> std::vector<Record>` and `GetFilePath()`; `GetDataBlocking()` defaults to `GetData()`. `GetDisplayName()` defaults to empty ("no label of its own" — the host names the source after `GetFilePath()` instead); only a source with no file behind it, i.e. `ManualDataSource`, overrides it. A source knows nothing about any `Title`, only `TitleRef{id, name}` copies handed to it. The optional hooks — all no-ops by default, only `ScriptDataSource` overrides them: `PumpEvents()` (per-tick housekeeping), `DrainOutTriggerRequests()` (Title uuids a script asked to hide via `trigger_out(...)`; empty-string sentinel = "every title reading this source"), `NotifyTriggerIn`/`NotifyTriggerOut(TitleRef, ...)`, and `SetTitleDirectory(std::vector<TitleRef>)` (the Scene's titles, for `scene.find_titles`).

- **`ManualDataSource`** — an operator-typed table instead of a file: `Table{name, columns, rows}`, where each `Column{name, type}` names the element id a cell is applied to (`ColumnType::Text`/`Image` is metadata for the host's editor UI only — the engine treats every cell as a string; an "image" cell is just a path `ImageElement::ApplyContent` assigns to `image_path`, there is no engine-side image handling). `GetData()` builds one `Record` per row, skipping columns with an empty name (they can't match an element id) and reading a short row's missing trailing cells as `""`; duplicate column names collapse, since `Record` is an `unordered_map`. `GetTable()`/`SetTable()` copy the table in and out under `m_mutex`. It is the first data source with mutable state living inside the object rather than on disk, and so the first to need its own mutex: `DataPool::Tick` calls `GetData()` on the render thread every 0.25s independently of the host UI thread calling `SetTable()`, and `DataPool`'s own mutex (see below) guards only its registry and caches, not what a source keeps inside itself. `ScriptDataSource` has the same problem and solves it the same way, with its own `m_dataMutex`. The mutex makes `ManualDataSource` non-copyable and non-movable, which is fine — `DataPool` only ever holds sources through `std::unique_ptr<IDataSource>`.

**`DataPool`** (`data-pool.h`) — Owns every `IDataSource` (`Add(source)` → returns its id / `Remove(id)` / `Clear()`), polls each on a fixed 0.25s cadence, and caches the result. That is the whole job: it never includes `title.h`, never holds a `Title*`, and never pushes anything into one. `Add` primes the cache with one synchronous fetch (so a host UI can preview a just-registered source), and the poll is unconditional — the pool has no idea what is on screen.
- **The changed flag** — each cache carries a `version`, bumped only when a refresh produced records that differ from what was cached (`0` = never successfully fetched). `Data(id)` / `DataVersion(id)` / `DataIfChanged(id, version&, out&)` are the read API; `DataIfChanged` compares versions for *inequality*, so a source removed and re-added (version restarting at 1) still reads as changed.
- `DataBlocking(id)` forces a fresh fetch. It copies out the raw `IDataSource*` under the mutex, **releases it**, and only then fetches — so a slow (seconds, for a network-backed script) fetch never stalls `Tick()`/`Add()`/`Remove()` on another thread.
- Source-facing relays, so nothing outside the pool handles a raw source pointer: `NotifyTriggerIn`/`NotifyTriggerOut(sourceId, TitleRef, ...)` (called by `Title`), `PublishTitleDirectory(titles)` and `DrainOutTriggerRequests()` → `{sourceId, titleUuids}` (called by `Scene`).
- **The title directory is tracked per source, not per change.** `Scene::Tick` hands `PublishTitleDirectory` the current directory every frame; the pool keeps the last one plus a version counter bumped only on a real change, and each `Entry` records the version *it* was handed. A source is pushed to when it is behind — so one registered after the directory last changed still gets it, which the older "only when the directory moved" gate silently never did (a reloaded script source sat on an empty directory forever, and its `scene.find_titles`/`scene.titles` quietly saw an empty scene). `Add` also hands the directory over at registration, shrinking the window for a replacement's worker; it can't close it, since that worker starts in the `ScriptDataSource` constructor, before `Add` is called.
- A throwing `GetData()` (a file source on a missing/malformed file) is caught in `Tick`/`Add`/`DataBlocking`: `Tick` runs on the render thread called from C code (libobs), where an escaping exception is `std::terminate`. On failure the previous cache is kept, never clobbered with `{}`, and the failure/recovery is logged once at the edge rather than every 0.25s.
- Removed/replaced sources are destroyed only **after** the mutex is released — `~ScriptDataSource` joins a worker thread that can block for seconds.

**`Scene`** (`scene.h`) — Owns the open `Title`s (`AddTitle`/`RemoveTitle`/`Clear`/`Titles`) plus the `DataPool` behind them (`Pool()`), and is the only place that knows both halves. `AddTitle` points the Title at the pool and guarantees id uniqueness. `FindByName(name)` returns every match; `FindById(uuid)` is exact. `Tick(dt)` runs, in order: (a) `DataPool::Tick`; (b) hand the `{id, name}` directory to the pool, which decides per source which sources are behind and need it (see `DataPool` above — a gate here couldn't see a source registered since the last change); (c) drain script-originated `trigger_out(...)` requests and apply them — a uuid hits that Title (any Title in the scene, not just one reading that source), the empty-string sentinel hits every Title reading *that* source; deduplicated within one drain and skipped for a Title already `Hidden`/`AnimatingOut`, so a doubled request can't replay an out-animation or double-relay `_on_trigger_out`; (d) `Title::Tick` on every Title. `Render(cr[, outW, outH])` draws non-`Hidden` Titles ordered by `Title::zOrder`. Persistence is deliberately the host's job — there is no `Scene::Save`/`Load`; the host rebuilds a Scene by adding sources first, then Titles carrying the matching `dataSourceId`.

**Threading** — a `Scene` and its `Title`s are single-threaded: `Scene::Tick`/`Render`/`AddTitle`/`RemoveTitle` and any direct `Title` call belong to the same (host render) thread. `DataPool`'s mutex protects only its own registry and caches, so a host UI thread may still `Add`/`Remove` sources; the two paths that must never hold it across a slow call — `DataBlocking()`'s fetch and source destruction — are documented above.

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

On `Title::Load`: bundled assets are extracted to a system temp directory for the life of the Title object. The directory is unique to that Title *instance* (a `"ogt-<stem>-<uuid>"` name, one fresh uuid per Load call), not derived from the `.ogt` path — two Titles loaded from the same file, or the same file loaded twice, never share one. On `Title::~Title`: the temp directory is removed. Sharing a directory between instances would be a bug: a host that reloads a Title loads the replacement before destroying the original, and the original's destructor would delete assets the replacement is still using.

On `Title::Save`: all referenced asset files (whether `@`-prefixed or local paths) are copied into `ASSETS/` and paths are rewritten as `@basename` in the JSON.

## title.json schema

```json
{
  "schema_version": { "major": 1, "minor": 1 },
  "version": 1,
  "id": "9f2c41e8-3b7a-4c1d-8e55-1a2b3c4d5e6f",
  "name": "lower_third",
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

The title-level `"id"` is the Title's uuid and `"name"` its human-readable name (since schema 1.1 — before that, `"id"` held the name). Element-level `"id"` is unchanged: the plain element id data records are matched against. The title↔data-source mapping is **not** serialized; the host owns it.

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

**Version constants** — `kOgtSchemaMajor` / `kOgtSchemaMinor` (currently **1.1**).
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
transforms the JSON in place from one version to the next). Current steps:
- `{0,0}` — legacy/absent-version normalization → 1.0 shape. A documented no-op (1.0 was
  purely additive over the unversioned baseline).
- `{1,0} → {1,1}` — the title-level `"id"` changed meaning: it used to hold the
  human-readable name, and now holds the Title's uuid. The step detects a pre-1.1 file by
  the *shape* of `"id"` (anything that isn't a v4 uuid is a legacy name), moves it to
  `"name"`, and generates a uuid — so it stays correct even if applied twice.

To add a migration: append a step keyed by the source version that walks the JSON
(recursing into `elements` and their children for element-field changes) and rewrites it
to the next version's shape.

Note the 1.1 wart: a 1.0-era reader opening a 1.1 file shows the uuid where it used to
show the name. That's cosmetic — unknown-field preservation means `"name"` still
round-trips untouched.

**Load diagnostic** — instead of printing to `stderr`, `Load` records a
`Title::LoadDiagnostic { Severity{None,Info,Warning}, message }`, readable via
`Title::GetLoadDiagnostic()`. The editor surfaces it as a non-blocking notice when a file
was created with a newer schema version.

## Lua scripting (`ScriptDataSource`)

Built only when the CMake option `ENABLE_LUA_SCRIPTING` (default `ON`) is enabled; when disabled, `script.h/cpp` are excluded from the `engine` target and Lua/sol2 are never fetched. `engine` publicly defines `ENGINE_HAS_LUA_SCRIPTING` only when the feature is built, so a consumer can guard any conditional `#include "engine/script.h"` with `#ifdef ENGINE_HAS_LUA_SCRIPTING`.

`ScriptDataSource` (`script.h/cpp`) runs a Lua 5.4 script (via sol2) as an `IDataSource`. It never sees a `Title` object — only `TitleRef{id, name}` copies, which surface in Lua as a **title handle**: a plain table `{ id = "<uuid>", name = "<name>" }`. Contract, all optional except `_get_data`:

| Lua global | Direction | Called |
|---|---|---|
| `_get_data()` | script → engine | Every `GetData()` pull (a blocking fetch via `DataPool::DataBlocking` when a Title is triggered in, or the 0.25s poll in `DataPool::Tick`). Must return a table of record-tables (string/number/bool values, converted to strings). |
| `_on_trigger_in(title, recordIndex, duration)` / `_on_trigger_out(title)` | engine → script | Whenever a `Title` reading this source is triggered in/out, from **any** origin (host call, `duration` timeout, or this script's own `trigger_out`) — relayed by `Title::TriggerIn`/`TriggerOut` through `DataPool::NotifyTriggerIn`/`NotifyTriggerOut`. `title` is the handle, so a script feeding several titles can tell them apart by `title.id` (unique) or `title.name`. |
| `trigger_out(title)` | script → engine | Accepts a handle (from `scene.find_titles`), a raw uuid string, or no argument at all — the bare `trigger_out()` form meaning "every title reading this source". Queues the uuid (or the empty-string sentinel) in a mutex-guarded list that `DataPool::Tick` exposes via `DrainOutTriggerRequests()` and `Scene::Tick` applies as `TriggerOut()` on the matching `Title`(s), on the host thread. A named handle can target **any** Title in the Scene, not just one reading this source. |

`trigger_out` and the `scene` table are bound into the Lua globals on the worker thread **after** `safe_script_file` — so they are callable from `_get_data`/`_on_trigger_*`, but *not* at script load time.

There is deliberately **no `trigger_in`** counterpart: whether a title is on-screen at all is the host's call, so a script asking to be *shown* isn't something the engine can honour meaningfully. Scripts still learn about every show through `_on_trigger_in`.

These are two independent, one-directional channels — `_on_trigger_in`/`_on_trigger_out` do not imply `trigger_out` and vice versa. See `tests/test_script_trigger.cpp` for exercised examples of each, including two Titles reading one `ScriptDataSource`.

### Injected Lua globals

Three data tables are registered on the `sol::state` in `ScriptDataSource::WorkerMain`, after `open_libraries()` and **before** `safe_script_file()`, so they are usable anywhere in the script rather than only inside `_get_data`:

| Table | File | Functions |
|---|---|---|
| `http` | `lua_http.h/cpp` | `get`, `post`, `put`, `patch`, `delete_` — blocking libcurl requests; each returns `{ok, status, body, error_msg, headers}`. Has a process-global `SetPermissionGate` hook. |
| `json` | `lua_json.h/cpp` | `decode`, `encode`, `null`, `array` — over nlohmann/json |
| `xml` | `lua_xml.h/cpp` | `decode`, `find`, `find_all` — over pugixml |

A fourth, `scene`, is registered later (with `trigger_out`, after `safe_script_file`) because it reads engine state rather than being a pure function:

| Table | Functions |
|---|---|
| `scene` | `find_titles(name)` → array of title handles with that name (may be empty or hold several — names aren't unique); `titles()` → every handle in the Scene |

Both read a snapshot `Scene::Tick` publishes into the source via `IDataSource::SetTitleDirectory` (only when the directory actually changed), guarded by `m_titleDirMutex` — so the worker thread never touches a live `Title`:

```lua
for _, t in ipairs(scene.find_titles("ticker")) do
    trigger_out(t)
end
```

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
