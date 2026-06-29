// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Visual test: mask clipping with a sheared element.
// Expected:
//   - The large orange rectangle is visible only inside the green box (the mask).
//   - The orange rectangle's shear is applied to its content, but the clip boundary
//     remains the green box (axis-aligned) — the mask must NOT be sheared.
//   - The drop shadow is also clipped to the green box boundary.
//
// The green box renders on top (zOrder=1) as a reference overlay.

#include "title.h"
#include "element_rectangle.h"
#include "element_text.h"
#include "sdl_runner.h"

#include <memory>

namespace {

Title buildTitle()
{
    Title t;
    t.width  = 1280;
    t.height = 720;

    // The mask element: a green rectangle that defines the clip boundary.
    // It also renders visually as a semi-transparent green overlay (zOrder=1).
    RectangleElement* maskPtr = nullptr;
    {
        auto el = std::make_unique<RectangleElement>();
        el->SetId("mask");
        el->SetBounds({390.0, 260.0, 500.0, 200.0});
        el->fill        = Paint::Solid(0.20, 0.85, 0.35, 0.20);
        el->stroke      = Paint::Solid(0.20, 0.85, 0.35, 0.85);
        el->strokeWidth = 2.0f;
        el->zOrder      = 1;
        maskPtr = el.get();
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    // Large sheared orange rectangle clipped to the green mask.
    // Extends well beyond the mask on all sides to make clipping obvious.
    {
        auto el = std::make_unique<RectangleElement>();
        el->SetId("sheared");
        el->SetBounds({190.0, 160.0, 900.0, 400.0});
        el->fill        = Paint::Solid(0.95, 0.50, 0.10);
        el->SetShearX(0.5f);
        el->shadow.enabled  = true;
        el->shadow.blur     = 25.0;
        el->shadow.offsetX  = 14.0;
        el->shadow.offsetY  = 14.0;
        el->shadow.color[3] = 0.80f;
        el->mask    = maskPtr;
        el->zOrder  = 0;
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    // Info label at top
    {
        auto el = std::make_unique<TextElement>();
        el->SetId("info");
        el->SetBounds({0.0, 20.0, 1280.0, 26.0});
        el->text = "Mask Test: orange rect (shearX=0.5) clipped to green box — clip boundary and shadow must not be sheared   [Esc]";
        el->font.family      = "Sans";
        el->font.size        = 13.5f;
        el->fill             = Paint::Solid(0.50, 0.50, 0.55);
        el->textStyle.alignX = HorizontalAlignment::Center;
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    t.state = TitleState::Visible;
    return t;
}

} // namespace

int main()
{
    SDLRunner app(1280, 720, "Mask + Shear Test");
    Title title = buildTitle();

    app.run([&](cairo_t* cr, float) {
        cairo_set_source_rgb(cr, 0.11, 0.11, 0.17);
        cairo_paint(cr);
        title.Render(cr);
    });
}
