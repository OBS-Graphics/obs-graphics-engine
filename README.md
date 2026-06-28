# obs-graphics-engine

Cairo/Pango rendering engine for animated broadcast graphics. A static C++20 library used by both the [obs-graphics](https://github.com/OBS-Graphics/obs-graphics) OBS plugin and the [obs-graphics-editor](https://github.com/OBS-Graphics/obs-graphics-editor) standalone editor.

## Dependencies

### System (must be installed)

| Package | Ubuntu/Debian | Fedora |
|---|---|---|
| Cairo + Pango | `libcairo2-dev libpango1.0-dev` | `cairo-devel pango-devel` |
| minizip | `libminizip-dev` | `minizip-devel` |
| CMake ≥ 3.16 | `cmake` | `cmake` |

```bash
# Ubuntu / Debian
sudo apt-get install libcairo2-dev libpango1.0-dev libminizip-dev cmake

# Fedora
sudo dnf install cairo-devel pango-devel minizip-devel cmake
```

### Fetched automatically by CPM at configure time

| Library | Version | Purpose |
|---|---|---|
| [lua](https://github.com/lua/lua) | 5.4.6 | Scripting runtime |
| [sol2](https://github.com/ThePhD/sol2) | v3.5.0 | C++ Lua bindings |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.12.0 | JSON parsing/serialization |
| [nothings/stb](https://github.com/nothings/stb) | master | Image loading (stb_image) |
| [yhirose/cpp-zipper](https://github.com/yhirose/cpp-zipper) | master | ZIP I/O for `.ogt` files (wraps system minizip) |

No manual setup needed — CPM downloads and configures all of these on first configure.

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
| `IDataSource` | Interface for JSON/CSV/Lua data sources bound to a Title. |

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

While `Visible`, `Tick()` also advances per-element data-change animations driven by the bound `IDataSource`.
