// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Visual test: clipChildren clipping with a sheared element.
// Expected:
//   - The large orange rectangle is visible only inside the green box (the clip parent).
//   - The orange rectangle's shear is applied to its content, but the clip boundary
//     is the green box (with its own transform applied) — matching the parent's shape.
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

    // Clip parent: a green rectangle that clips its children and also renders visibly.
    {
        auto el = std::make_unique<RectangleElement>();
        el->SetId("clip_box");
        el->SetBounds({390.0, 260.0, 500.0, 200.0});
        el->fill         = Paint::Solid(0.20, 0.85, 0.35, 0.20);
        el->stroke       = Paint::Solid(0.20, 0.85, 0.35, 0.85);
        el->strokeWidth  = 2.0f;
        el->zOrder       = 1;
        el->clipChildren = true;

        // Large sheared orange rectangle — bounds relative to green box.
        // Extends well beyond the parent on all sides to make clipping obvious.
        {
            auto child = std::make_unique<RectangleElement>();
            child->SetId("sheared");
            child->SetBounds({-200.0, -100.0, 900.0, 400.0});
            child->fill         = Paint::Solid(0.95, 0.50, 0.10);
            child->SetShearX(0.5f);
            child->shadow.enabled  = true;
            child->shadow.blur     = 25.0;
            child->shadow.offsetX  = 14.0;
            child->shadow.offsetY  = 14.0;
            child->shadow.color[3] = 0.80f;
            child->zOrder          = 0;
            el->AddChild(child.get());
            t.elements.push_back(std::move(child));
        }

        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    // Info label at top
    {
        auto el = std::make_unique<TextElement>();
        el->SetId("info");
        el->SetBounds({0.0, 20.0, 1280.0, 26.0});
        el->text = "clipChildren Test: orange rect (shearX=0.5) clipped to green box — shadow also clipped   [Esc]";
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
    SDLRunner app(1280, 720, "clipChildren + Shear Test");
    Title title = buildTitle();

    app.run([&](cairo_t* cr, float) {
        cairo_set_source_rgb(cr, 0.11, 0.11, 0.17);
        cairo_paint(cr);
        title.Render(cr);
    });
}
