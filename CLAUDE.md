# CLAUDE.md

Rendering engine for obs-graphics. Static C++20 library. No Qt — renders via Cairo/Pango only.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

sol2, cpp-zipper, and nlohmann/json are fetched automatically by CPM at configure time. cpp-zipper wraps system minizip (`libminizip-dev` must be installed).

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
- `render_util.h/cpp` — `namespace render`: `RoundRect`, `LoadImageSurface`, `RenderDropShadow`, `RenderDropShadowFromSurface` (box-blur shadow pipeline)
- `animation.h/cpp` — `AnimationDef`, `AnimatedTransform`, easing functions
- `types.hpp` — `Paint`, `Point`, `Size`, `Rectangle`, `ScaleMode` — core shared types
- `data-source.h/cpp` — `IDataSource`, JSON/CSV data source implementations
- `script.h/cpp` — `ScriptDataSource` (Lua via sol2)
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

**`Spatial`** — Adds `m_bounds` (local position + size), `m_rotation`, `m_shearX/Y`. Overrides `SetParent` to adjust `m_bounds.x/y` so the element's **world position is preserved** when reparented: saves `GetGlobalPosition()` before changing parent, then subtracts the new parent's world position. When loading from JSON (where x/y are local), `title.cpp` temporarily converts to world coords before calling `AddChild` so SetParent's subtraction is a net no-op.

**`VisualElement`** — All renderable properties: `fill`, `stroke`, `strokeWidth`, `cornerRadius[4]`, `opacity`, `mask`, `shadow{}`, `inAnimation`/`outAnimation`, `dataInAnimation`/`dataOutAnimation`, `zOrder`, `fitToChildren`. Provides `Render()` (common pipeline: shadow → wipe clip → opacity group → shear → content → children). `SetContent(value)` is **final** — it is a no-op if the value hasn't changed, otherwise drives the data-change animation state machine and calls `ApplyContent(value)` at the right moment. Subclasses override `ApplyContent` instead. Private helpers `ComposeDataAnimation`, `RenderShadow`, `RenderChildren` keep `Render` concise. Subclasses may override `CreateShadowSurface(w, h) → cairo_surface_t*` to supply a custom A8 shadow mask (nullptr = default rounded-rect shadow).

**`Paint`** — Fill or stroke. `type` = Solid / Linear / Radial / Image. Params layout:
- Solid: `params[0..3]` = r,g,b,a
- Linear: `params[0..3]` = x0,y0,x1,y1 (normalized 0–1, scaled by element bounds at render)
- Radial: `params[0..2]` = focus cx,cy,r; `params[3..5]` = main cx,cy,r (normalized by element width)
- Image: loaded via `Paint::Image(path)` using `cairo_image_surface_create_from_png`

**`namespace render`** (`render_util.h/cpp`) — Stateless utilities:
- `RoundRect(ctx, x, y, w, h, r[4])` — rounded-rect path
- `LoadImageSurface(path)` — stb_image → premultiplied ARGB32 Cairo surface
- `RenderDropShadow(...)` — 3× separable box-blur (≈Gaussian) of a rounded-rect shape on an A8 surface, composited as a coloured shadow via `cairo_mask_surface()`. O(pixels) regardless of blur radius. `blur` maps directly to Gaussian sigma via `GaussBoxRadii`.
- `RenderDropShadowFromSurface(ctx, srcA8, destX, destY, blur, r, g, b, a)` — same blur pipeline but takes a pre-rendered A8 surface as the shadow shape. Used by `TextElement` to cast text-shaped shadows.

**`Title`** — Standalone presentation unit. Holds `width`/`height`, `metadata` (arbitrary JSON), an elements vector (root at [0], VisualElements at [1..]), and a state machine: `Hidden → AnimatingIn → Visible → AnimatingOut → Hidden`. `Tick(dt)` advances the animation timer and, while Visible, calls `TickData(dt)` on each VisualElement to drive per-element data-change animations. `Render(cr)` draws direct children of root sorted by `zOrder`. Loads/saves as `.ogt` (zip archive, see below).

**`AnimationDef` / `AnimatedTransform`** — In/out animation per element. `animation::EvaluateAnimation(def, t, isOut, width, height)` returns an `AnimatedTransform` (offset, scale, opacity, clip rect). Out animations are true reversals. Per-element data-change animations are stored in `dataInAnimation`/`dataOutAnimation` and driven by `VisualElement::TickData`.

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
| `shear_x` | float | Horizontal shear factor (default 0) |
| `shear_y` | float | Vertical shear factor (default 0) |
| `shadow` | object | Drop shadow: `enabled` (bool), `offset_x`, `offset_y`, `blur` (floats), `color` ([r,g,b,a] array) |
| `mask` | string | id of another element to use as a clip mask |
| `parent` | string | id of parent element; omit to parent directly to root |
| `anim_in` / `anim_out` | object | In/out animation def: `type`, `easing`, `duration`, `delay` |
| `data_anim_in` / `data_anim_out` | object | Data-change animation (same structure as anim_in/out) |
| `metadata` *(title-level)* | object | Arbitrary key-value store for editor/consumer settings; omitted when empty |

## Conventions

- Gradient coordinates in `Paint` are normalized 0–1 and scaled by element bounds at Cairo render time.
- Mask clipping offset is computed via `GetGlobalPosition()` on both elements — never via raw bounds — so clipping is correct when either element has a parent.
- Image element always clips to its own bounds before painting, so Cover/FitWidth/FitHeight/None scale modes cannot bleed outside the element rect.
- Drop shadow is drawn before any wipe/corner/mask clip; during wipe animations it tracks the clip box rather than full element bounds.
- `TextElement` casts a text-shaped shadow (actual glyph outlines blurred) via `CreateShadowSurface`. During wipe animations the shadow falls back to the bounding-rect shape.
- Data-change animations compose on top of graphic in/out animations: opacities multiply, offsets add, scales multiply. A wipe data animation overrides the graphic wipe clip.
- `SOL_NO_THREAD_LOCAL` is defined to avoid Lua thread-local overhead.
- `-fPIC` is required because this static lib is linked into a shared library (OBS module).

## Include path for consumers

The CMakeLists exports `${CMAKE_CURRENT_SOURCE_DIR}/..` as the PUBLIC include directory. When this repo is a submodule at `engine/`, consumers include as:

```cpp
#include "engine/title.h"
#include "engine/types.hpp"
// etc.
```
