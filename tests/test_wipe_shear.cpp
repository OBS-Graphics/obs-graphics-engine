// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Visual test: wipe animations with shadows.
//
// Row 0 — rectangles, ShearX=0.4 (WipeRight | WipeLeft)
//   Expected: wipe edge is slanted, shadow tracks the wipe box.
//
// Row 1 — text elements, no shear (WipeRight | WipeLeft)
//   Expected: text-shaped shadow reveals/hides with the wipe — no pop.
//
// Row 2 — rectangles, ShearY=0.35 (WipeDown | WipeUp)
//   Expected: wipe edge is slanted, shadow tracks the wipe box.

#include "title.h"
#include "element_rectangle.h"
#include "element_text.h"
#include "sdl_runner.h"

#include <memory>
#include <string>

namespace {

// ── Layout ───────────────────────────────────────────────────────────────────

constexpr double kColX[2]  = {90.0,  690.0};
constexpr double kRectW    = 500.0, kRectH = 90.0;
constexpr double kTextW    = 500.0, kTextH = 65.0;
constexpr double kRowRect0 = 175.0;   // row 0: shear-X rects
constexpr double kRowText  = 375.0;   // row 1: text shadow
constexpr double kRowRect1 = 540.0;   // row 2: shear-Y rects

// ── Helpers ───────────────────────────────────────────────────────────────────

static void addLabel(Title& t, const std::string& id,
                     double x, double y, double w,
                     const char* text, bool bold = false)
{
    auto el = std::make_unique<TextElement>();
    el->SetId(id);
    el->SetBounds({x, y, w, 20.0});
    el->text            = text;
    el->font.family     = "Sans";
    el->font.size       = 12.5f;
    el->font.weight     = bold ? FontWeight::Bold : FontWeight::Normal;
    el->fill            = bold ? Paint::Solid(0.82, 0.82, 0.88)
                               : Paint::Solid(0.48, 0.48, 0.56);
    t.GetRoot()->AddChild(el.get());
    t.elements.push_back(std::move(el));
}

static void addRect(Title& t, const std::string& id,
                    double x, double y, double w, double h,
                    float shX, float shY, const Paint& color,
                    AnimationType animType)
{
    auto el = std::make_unique<RectangleElement>();
    el->SetId(id);
    el->SetBounds({x, y, w, h});
    for (int k = 0; k < 4; ++k) el->cornerRadius[k] = 5.0f;
    el->fill = color;
    el->SetShearX(shX);
    el->SetShearY(shY);
    el->shadow.enabled  = true;
    el->shadow.blur     = 14.0;
    el->shadow.offsetX  = 8.0;
    el->shadow.offsetY  = 8.0;
    el->shadow.color[3] = 0.65f;
    el->inAnimation  = {animType, Easing::EaseInOutCubic, 1.20f, 0.0f};
    el->outAnimation = {animType, Easing::EaseInOutCubic, 1.20f, 0.0f};
    t.GetRoot()->AddChild(el.get());
    t.elements.push_back(std::move(el));
}

static void addTextShadow(Title& t, const std::string& id,
                           double x, double y, double w, double h,
                           AnimationType animType)
{
    auto el = std::make_unique<TextElement>();
    el->SetId(id);
    el->SetBounds({x, y, w, h});
    el->text            = "Drop Shadow";
    el->font.family     = "Sans";
    el->font.size       = 34.0f;
    el->font.weight     = FontWeight::Bold;
    el->fill            = Paint::Solid(0.95, 0.95, 1.00);
    el->shadow.enabled  = true;
    el->shadow.blur     = 10.0;
    el->shadow.offsetX  = 5.0;
    el->shadow.offsetY  = 5.0;
    el->shadow.color[3] = 0.80f;
    el->inAnimation  = {animType, Easing::EaseInOutCubic, 1.20f, 0.0f};
    el->outAnimation = {animType, Easing::EaseInOutCubic, 1.20f, 0.0f};
    t.GetRoot()->AddChild(el.get());
    t.elements.push_back(std::move(el));
}

// ── Scene ─────────────────────────────────────────────────────────────────────

Title buildTitle()
{
    Title t;
    t.width  = 1280;
    t.height = 720;

    // Header
    {
        auto el = std::make_unique<TextElement>();
        el->SetId("header");
        el->SetBounds({0.0, 18.0, 1280.0, 26.0});
        el->text = "Wipe + Shadow Test   [Esc to quit]";
        el->font.family      = "Sans";
        el->font.size        = 14.5f;
        el->fill             = Paint::Solid(0.42, 0.42, 0.50);
        el->textStyle.alignX = HorizontalAlignment::Center;
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    // Row 0: rectangles with ShearX
    const Paint kBlue  = Paint::Solid(0.22, 0.47, 0.82);
    const Paint kGreen = Paint::Solid(0.18, 0.68, 0.44);
    addLabel(t, "n0", kColX[0], kRowRect0 - 50.0, kRectW, "WipeRight + ShearX=0.4", true);
    addLabel(t, "d0", kColX[0], kRowRect0 - 27.0, kRectW, "slanted wipe edge, shadow tracks box");
    addRect (t, "r0", kColX[0], kRowRect0, kRectW, kRectH, 0.4f, 0.0f, kBlue,  AnimationType::WipeRight);

    addLabel(t, "n1", kColX[1], kRowRect0 - 50.0, kRectW, "WipeLeft + ShearX=0.4", true);
    addLabel(t, "d1", kColX[1], kRowRect0 - 27.0, kRectW, "slanted wipe edge, shadow tracks box");
    addRect (t, "r1", kColX[1], kRowRect0, kRectW, kRectH, 0.4f, 0.0f, kGreen, AnimationType::WipeLeft);

    // Row 1: text shadow
    addLabel(t, "n2", kColX[0], kRowText - 50.0, kTextW, "WipeRight — text shadow", true);
    addLabel(t, "d2", kColX[0], kRowText - 27.0, kTextW, "glyph shadow reveals with wipe, no pop");
    addTextShadow(t, "t0", kColX[0], kRowText, kTextW, kTextH, AnimationType::WipeRight);

    addLabel(t, "n3", kColX[1], kRowText - 50.0, kTextW, "WipeLeft — text shadow", true);
    addLabel(t, "d3", kColX[1], kRowText - 27.0, kTextW, "glyph shadow reveals with wipe, no pop");
    addTextShadow(t, "t1", kColX[1], kRowText, kTextW, kTextH, AnimationType::WipeLeft);

    // Row 2: rectangles with ShearY
    const Paint kOrange = Paint::Solid(0.82, 0.53, 0.14);
    const Paint kRose   = Paint::Solid(0.72, 0.24, 0.37);
    addLabel(t, "n4", kColX[0], kRowRect1 - 50.0, kRectW, "WipeDown + ShearY=0.35", true);
    addLabel(t, "d4", kColX[0], kRowRect1 - 27.0, kRectW, "slanted wipe edge, shadow tracks box");
    addRect (t, "r2", kColX[0], kRowRect1, kRectW, kRectH, 0.0f, 0.35f, kOrange, AnimationType::WipeDown);

    addLabel(t, "n5", kColX[1], kRowRect1 - 50.0, kRectW, "WipeUp + ShearY=0.35", true);
    addLabel(t, "d5", kColX[1], kRowRect1 - 27.0, kRectW, "slanted wipe edge, shadow tracks box");
    addRect (t, "r3", kColX[1], kRowRect1, kRectW, kRectH, 0.0f, 0.35f, kRose,   AnimationType::WipeUp);

    // Footer hint
    {
        auto el = std::make_unique<TextElement>();
        el->SetId("hint");
        el->SetBounds({0.0, 662.0, 1280.0, 20.0});
        el->text = "Shadow should grow/shrink with the wipe — no sudden pop at start or end.";
        el->font.family      = "Sans";
        el->font.size        = 11.5f;
        el->fill             = Paint::Solid(0.35, 0.35, 0.42);
        el->textStyle.alignX = HorizontalAlignment::Center;
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    return t;
}

} // namespace

int main()
{
    SDLRunner app(1280, 720, "Wipe + Shadow Test");
    Title title = buildTitle();
    title.TriggerIn();

    TitleState prevState = TitleState::Hidden;
    float visibleTimer = -1.0f;
    float hiddenTimer  = -1.0f;

    app.run([&](cairo_t* cr, float dt) {
        title.Tick(dt);
        TitleState cur = title.state;

        if (prevState == TitleState::AnimatingIn && cur == TitleState::Visible)
            visibleTimer = 1.0f;

        if (cur == TitleState::Visible && visibleTimer >= 0.0f) {
            visibleTimer -= dt;
            if (visibleTimer < 0.0f)
                title.TriggerOut();
        }

        if (prevState == TitleState::AnimatingOut && cur == TitleState::Hidden)
            hiddenTimer = 0.5f;

        if (hiddenTimer >= 0.0f) {
            hiddenTimer -= dt;
            if (hiddenTimer < 0.0f) {
                title.TriggerIn();
                hiddenTimer = -1.0f;
            }
        }

        prevState = cur;

        cairo_set_source_rgb(cr, 0.09, 0.10, 0.14);
        cairo_paint(cr);
        title.Render(cr);
    });
}
