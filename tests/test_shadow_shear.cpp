// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Visual test: drop shadows on rectangles with various shear/rotation values.
// Expected: shadow shape follows the transform — sheared rects have sheared shadows,
// rotated rects have rotated shadows. Corner radius is preserved in the shadow shape.

#include "title.h"
#include "element_rectangle.h"
#include "element_text.h"
#include "sdl_runner.h"

#include <memory>
#include <string>

namespace {

struct Config { const char* label; float shX, shY, rot; };

constexpr Config kCfg[6] = {
    {"No transform",                     0.0f,  0.0f,  0.0f},
    {"shearX = 0.5",                     0.5f,  0.0f,  0.0f},
    {"shearX=0.5  rot=20\xc2\xb0",       0.5f,  0.0f, 20.0f},
    {"shearY = 0.4",                     0.0f,  0.4f,  0.0f},
    {"rotation = 35\xc2\xb0",            0.0f,  0.0f, 35.0f},
    {"shX=0.3  shY=0.2  rot=10\xc2\xb0", 0.3f,  0.2f, 10.0f},
};

// Rect left-edge X per column, top-edge Y per row (rect size 200×90)
constexpr double kColX[3] = {113.0, 540.0, 967.0};
constexpr double kRowY[2] = {185.0, 475.0};
constexpr double kRW = 200.0, kRH = 90.0;

Title buildTitle()
{
    Title t;
    t.width  = 1280;
    t.height = 720;

    // Page header
    {
        auto el = std::make_unique<TextElement>();
        el->SetId("header");
        el->SetBounds({0.0, 18.0, 1280.0, 28.0});
        el->text = "Shadow + Shear / Rotation Test   [Esc to quit]";
        el->font.family  = "Sans";
        el->font.size    = 15.0f;
        el->fill         = Paint::Solid(0.45, 0.45, 0.52);
        el->textStyle.alignX = HorizontalAlignment::Center;
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    auto addLabel = [&](const std::string& id, double x, double y, double w, const char* text) {
        auto el = std::make_unique<TextElement>();
        el->SetId(id);
        el->SetBounds({x, y, w, 20.0});
        el->text             = text;
        el->font.family      = "Sans";
        el->font.size        = 12.5f;
        el->fill             = Paint::Solid(0.60, 0.60, 0.65);
        el->textStyle.alignX = HorizontalAlignment::Center;
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    };

    auto addRect = [&](const std::string& id, Rectangle b,
                       float shX, float shY, float rot) -> RectangleElement* {
        auto el = std::make_unique<RectangleElement>();
        el->SetId(id);
        el->SetBounds(b);
        for (int i = 0; i < 4; ++i) el->cornerRadius[i] = 8.0f;
        el->fill = Paint::Solid(0.27, 0.45, 0.78);
        el->SetShearX(shX);
        el->SetShearY(shY);
        el->SetRotation(rot);
        el->shadow.enabled  = true;
        el->shadow.blur     = 18.0;
        el->shadow.offsetX  = 10.0;
        el->shadow.offsetY  = 10.0;
        el->shadow.color[3] = 0.75f;
        auto* ptr = el.get();
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
        return ptr;
    };

    for (int i = 0; i < 6; ++i) {
        int col = i % 3, row = i / 3;
        double rx = kColX[col], ry = kRowY[row];
        addLabel("lbl" + std::to_string(i), rx - 20.0, ry - 26.0, kRW + 40.0, kCfg[i].label);
        addRect("rect" + std::to_string(i), {rx, ry, kRW, kRH},
                kCfg[i].shX, kCfg[i].shY, kCfg[i].rot);
    }

    t.state = TitleState::Visible;
    return t;
}

} // namespace

int main()
{
    SDLRunner app(1280, 720, "Shadow + Shear Test");
    Title title = buildTitle();

    app.run([&](cairo_t* cr, float) {
        cairo_set_source_rgb(cr, 0.11, 0.11, 0.17);
        cairo_paint(cr);
        title.Render(cr);
    });
}
