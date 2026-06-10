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
- `element.h/cpp` — `Element` (Rectangle/Text) + Cairo rendering
- `animation.h/cpp` — `AnimationDef`, `AnimatedTransform`, easing functions
- `types.hpp` — `Paint`, `Point`, `Rectangle`, `GradientStop` — core shared types
- `data-source.h/cpp` — `IDataSource`, JSON/CSV data source implementations
- `script.h/cpp` — `ScriptDataSource` (Lua via sol2)
- `json.hpp` — vendored nlohmann/json

## Key types

**`Paint`** — Fill or stroke. `type` = Solid / Linear / Radial. Params layout:
- Solid: `params[0..3]` = r,g,b,a
- Linear: `params[0..3]` = x0,y0,x1,y1 (normalized 0–1, scaled by element bounds at render)
- Radial: `params[0..2]` = focus cx,cy,r; `params[3..5]` = main cx,cy,r (normalized by element width)

**`Element`** — Renderable item (Rectangle or Text).
- `bounds.x/y` are **local (parent-relative)** when `parent != nullptr`. Always use `GetGlobalPosition()` for screen position.
- `ApplyClipping()` clips to mask element bounds, accepts optional `AnimatedTransform*` so slide animations are accounted for.
- Z-order: `Graphic::Render()` sorts elements ascending by `zOrder` (lower = rendered first = behind).

**`Graphic`** — Named group of elements. State machine: `Hidden → AnimatingIn → Visible → AnimatingOut → Hidden`. `Tick(dt)` advances the timer; `Render(cr)` draws all elements sorted by zOrder.

**`Scene`** — `width`/`height` + `std::vector<Graphic>`. Serialized as `"width"`/`"height"` in JSON (default 1920×1080). Load via `Scene::Load(path)` or `Scene::LoadString(json)`.

**`AnimationDef` / `AnimatedTransform`** — In/out animation per element. `animation::EvaluateAnimation(def, t)` returns an `AnimatedTransform` (offset, scale, opacity) at time `t`.

## Conventions

- Gradient coordinates in `Paint` are normalized 0–1 and scaled by element bounds at Cairo render time.
- Mask clipping offset is computed via `GetGlobalPosition()` on both elements — never via raw `bounds` — so clipping is correct when either element has a parent.
- `SOL_NO_THREAD_LOCAL` is defined to avoid Lua thread-local overhead.
- `-fPIC` is required because this static lib is linked into a shared library (OBS module).

## Include path for consumers

The CMakeLists exports `${CMAKE_CURRENT_SOURCE_DIR}/..` as the PUBLIC include directory. When this repo is a submodule at `engine/`, consumers include as:

```cpp
#include "engine/scene.h"
#include "engine/types.hpp"
// etc.
```
