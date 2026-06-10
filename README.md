# obs-graphics-engine

Cairo/Pango rendering engine for animated broadcast graphics. A static C++20 library used by both the [obs-graphics](https://github.com/OBS-Graphics/obs-graphics) OBS plugin and the [obs-graphics-editor](https://github.com/OBS-Graphics/obs-graphics-editor) standalone editor.

## Dependencies

- Cairo + Pango (`libcairo2-dev`, `libpango1.0-dev`)
- Lua 5.x (`liblua5.4-dev`)
- sol2 v3.3.0 (fetched automatically via CPM)
- CMake 3.16+

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

Headers are then available as `#include "engine/scene.h"` etc.

## Core types

| Type | Description |
|------|-------------|
| `Scene` | Top-level container — width/height + vector of `Graphic`s. Loads from JSON via `Scene::Load()`. |
| `Graphic` | Named group of `Element`s with a state machine: Hidden → AnimatingIn → Visible → AnimatingOut. |
| `Element` | Rectangle or Text item with bounds, fill/stroke `Paint`, corner radius, opacity, rotation, in/out animations, optional parent/mask. |
| `Paint` | Solid, Linear, or Radial fill using normalized 0–1 coordinates. |
| `AnimationDef` | Per-element animation: type, easing, duration, delay. Evaluated by `animation::EvaluateAnimation()`. |

See `types.hpp` for full type definitions.
