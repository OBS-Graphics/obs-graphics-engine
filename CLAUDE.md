# CLAUDE.md

Rendering engine for obs-graphics. Static C++20 library. No Qt — renders via Cairo/Pango only.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

sol2 is fetched automatically by CPM at configure time.

## Source files

All sources live at repo root (no `src/` subdirectory):

- `scene.h/cpp` — `Scene` struct + JSON load/save
- `graphic.h/cpp` — `Graphic` state machine + render
- `element.h/cpp` — `IElement` base class (parent/child tree management)
- `spatial.h/cpp` — `Spatial : IElement` — adds bounds, rotation, shear, world-position tracking
- `visual_element.h/cpp` — `VisualElement : Spatial` — common rendering (shadow, clip, opacity group, children); pure virtual `RenderContent`
- `element_rectangle.h/cpp` — `RectangleElement : VisualElement`
- `element_text.h/cpp` — `TextElement : VisualElement`; also owns all text/font enums
- `element_image.h/cpp` — `ImageElement : VisualElement`
- `element_qr.h/cpp` — `QrElement : VisualElement`
- `render_util.h/cpp` — `namespace render`: `RoundRect`, `LoadImageSurface`, `RenderDropShadow` (box-blur shadow pipeline)
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

`Graphic::elements` is `std::vector<std::unique_ptr<VisualElement>>`.

## Key types

**`IElement`** — Base interface. Manages the parent/child tree via `AddChild`/`RemoveChild`/`SetParent`. `AddChild(child)` calls `child->SetParent(this)`; `RemoveChild(child)` calls `child->SetParent(nullptr)`. Internal list manipulation uses `AddChildDirect`/`RemoveChildDirect` to avoid recursion.

**`Spatial`** — Adds `m_bounds` (local position + size), `m_rotation`, `m_shearX/Y`. Overrides `SetParent` to adjust `m_bounds.x/y` so the element's **world position is preserved** when reparented: saves `GetGlobalPosition()` before changing parent, then subtracts the new parent's world position. When loading from JSON (where x/y are local), `scene.cpp` temporarily converts to world coords before calling `AddChild` so SetParent's subtraction is a net no-op.

**`VisualElement`** — All renderable properties: `fill`, `stroke`, `strokeWidth`, `cornerRadius[4]`, `opacity`, `mask`, `shadow{}`, `inAnimation`/`outAnimation`, `zOrder`, `fitToChildren`. Provides `Render()` (common pipeline: shadow → wipe clip → opacity group → shear → content → children) and abstract `RenderContent(ctx)`. `SetContent(value)` is virtual — overridden by TextElement (sets `text`), ImageElement (sets `imagePath`), QrElement (sets `text`); used by `Graphic::UpdateData` for data-source binding without type switches.

**`Paint`** — Fill or stroke. `type` = Solid / Linear / Radial / Image. Params layout:
- Solid: `params[0..3]` = r,g,b,a
- Linear: `params[0..3]` = x0,y0,x1,y1 (normalized 0–1, scaled by element bounds at render)
- Radial: `params[0..2]` = focus cx,cy,r; `params[3..5]` = main cx,cy,r (normalized by element width)
- Image: loaded via `Paint::Image(path)` using `cairo_image_surface_create_from_png`

**`namespace render`** (`render_util.h/cpp`) — Stateless utilities:
- `RoundRect(ctx, x, y, w, h, r[4])` — rounded-rect path
- `LoadImageSurface(path)` — stb_image → premultiplied ARGB32 Cairo surface
- `RenderDropShadow(...)` — 3× separable box-blur (≈Gaussian) of the element shape on an A8 surface, composited as a coloured shadow via `cairo_mask_surface()`. O(pixels) regardless of blur radius. `blur` maps directly to Gaussian sigma via `GaussBoxRadii`.

**`Graphic`** — Named group of elements. State machine: `Hidden → AnimatingIn → Visible → AnimatingOut → Hidden`. `Tick(dt)` advances the timer; `Render(cr)` draws root elements sorted by `zOrder`. `UpdateData()` calls `el->SetContent(value)` via virtual dispatch.

**`Scene`** — `width`/`height` + `std::vector<Graphic>`. Load via `Scene::Load(path)` or `Scene::LoadString(json)`.

**`AnimationDef` / `AnimatedTransform`** — In/out animation per element. `animation::EvaluateAnimation(def, t, isOut, width, height)` returns an `AnimatedTransform` (offset, scale, opacity, clip rect). Out animations are true reversals.

## JSON schema (element fields)

| Field | Type | Notes |
|---|---|---|
| `type` | string | `"rectangle"`, `"text"`, `"image"`, `"qr_code"` |
| `x`, `y`, `w`, `h` | float | Bounds — **local (parent-relative)** when `parent` is set |
| `image_path` | string | Image file path (PNG, JPEG, etc.) for Image element |
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
| `parent` | string | id of parent element (child `x/y` are then parent-relative) |

## Conventions

- Gradient coordinates in `Paint` are normalized 0–1 and scaled by element bounds at Cairo render time.
- Mask clipping offset is computed via `GetGlobalPosition()` on both elements — never via raw bounds — so clipping is correct when either element has a parent.
- Image element always clips to its own bounds before painting, so Cover/FitWidth/FitHeight/None scale modes cannot bleed outside the element rect.
- Drop shadow is drawn before any wipe/corner/mask clip; during wipe animations it tracks the clip box rather than full element bounds.
- `SOL_NO_THREAD_LOCAL` is defined to avoid Lua thread-local overhead.
- `-fPIC` is required because this static lib is linked into a shared library (OBS module).

## Include path for consumers

The CMakeLists exports `${CMAKE_CURRENT_SOURCE_DIR}/..` as the PUBLIC include directory. When this repo is a submodule at `engine/`, consumers include as:

```cpp
#include "engine/scene.h"
#include "engine/types.hpp"
// etc.
```
