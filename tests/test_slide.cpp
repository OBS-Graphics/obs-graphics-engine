// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Visual test: slide animation directions.
// Expected:
//   SlideLeft  — enters from the RIGHT, exits to the LEFT
//   SlideRight — enters from the LEFT, exits to the RIGHT
//   SlideUp    — enters from the BOTTOM, exits UPWARD
//   SlideDown  — enters from the TOP, exits DOWNWARD

#include "title.h"
#include "element_rectangle.h"
#include "element_text.h"
#include "sdl_runner.h"

#include <memory>
#include <string>

namespace {

struct SlideCase {
    const char*   name;
    const char*   desc;
    AnimationType type;
    Paint         color;
};

static const SlideCase kCases[4] = {
    {"SlideLeft",  "enters from RIGHT  \xe2\x86\x90  exits LEFT",
     AnimationType::SlideLeft,  Paint::Solid(0.22, 0.47, 0.82)},
    {"SlideRight", "enters from LEFT  \xe2\x86\x92  exits RIGHT",
     AnimationType::SlideRight, Paint::Solid(0.20, 0.70, 0.45)},
    {"SlideUp",    "enters from BOTTOM  \xe2\x86\x91  exits UP",
     AnimationType::SlideUp,    Paint::Solid(0.82, 0.55, 0.15)},
    {"SlideDown",  "enters from TOP  \xe2\x86\x93  exits DOWN",
     AnimationType::SlideDown,  Paint::Solid(0.72, 0.25, 0.38)},
};

// 2×2 grid on a 1280×720 canvas
constexpr double kRectW = 530.0, kRectH = 90.0;
constexpr double kColX[2] = {75.0,  675.0};
constexpr double kRowY[2] = {290.0, 490.0};

Title buildTitle()
{
    Title t;
    t.width  = 1280;
    t.height = 720;

    {
        auto el = std::make_unique<TextElement>();
        el->SetId("header");
        el->SetBounds({0.0, 20.0, 1280.0, 28.0});
        el->text = "Slide Direction Test   [Esc to quit]";
        el->font.family      = "Sans";
        el->font.size        = 15.0f;
        el->fill             = Paint::Solid(0.45, 0.45, 0.52);
        el->textStyle.alignX = HorizontalAlignment::Center;
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    {
        auto el = std::make_unique<TextElement>();
        el->SetId("hint");
        el->SetBounds({0.0, 660.0, 1280.0, 22.0});
        el->text = "Watch which side each rectangle enters/exits from.";
        el->font.family      = "Sans";
        el->font.size        = 12.5f;
        el->fill             = Paint::Solid(0.38, 0.38, 0.44);
        el->textStyle.alignX = HorizontalAlignment::Center;
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    for (int i = 0; i < 4; ++i) {
        const int col = i % 2, row = i / 2;
        const double rx = kColX[col], ry = kRowY[row];
        const auto& c = kCases[i];

        // Animation type label
        {
            auto el = std::make_unique<TextElement>();
            el->SetId("name" + std::to_string(i));
            el->SetBounds({rx, ry - 52.0, kRectW, 22.0});
            el->text = c.name;
            el->font.family = "Sans";
            el->font.size   = 14.0f;
            el->font.weight = FontWeight::Bold;
            el->fill        = Paint::Solid(0.85, 0.85, 0.90);
            t.GetRoot()->AddChild(el.get());
            t.elements.push_back(std::move(el));
        }

        // Direction description
        {
            auto el = std::make_unique<TextElement>();
            el->SetId("desc" + std::to_string(i));
            el->SetBounds({rx, ry - 27.0, kRectW, 18.0});
            el->text = c.desc;
            el->font.family = "Sans";
            el->font.size   = 11.5f;
            el->fill        = Paint::Solid(0.50, 0.50, 0.58);
            t.GetRoot()->AddChild(el.get());
            t.elements.push_back(std::move(el));
        }

        // Animated rectangle — same type used for both in and out
        {
            auto el = std::make_unique<RectangleElement>();
            el->SetId("rect" + std::to_string(i));
            el->SetBounds({rx, ry, kRectW, kRectH});
            for (int k = 0; k < 4; ++k) el->cornerRadius[k] = 6.0f;
            el->fill         = c.color;
            el->inAnimation  = {c.type, Easing::EaseOutCubic, 0.60f, 0.0f};
            el->outAnimation = {c.type, Easing::EaseInCubic,  0.60f, 0.0f};
            t.GetRoot()->AddChild(el.get());
            t.elements.push_back(std::move(el));
        }
    }

    return t;
}

} // namespace

int main()
{
    SDLRunner app(1280, 720, "Slide Direction Test");
    Title title = buildTitle();
    title.TriggerIn();

    TitleState prevState = TitleState::Hidden;
    float visibleTimer = -1.0f;
    float hiddenTimer  = -1.0f;

    app.run([&](cairo_t* cr, float dt) {
        title.Tick(dt);
        TitleState cur = title.state;

        if (prevState == TitleState::AnimatingIn && cur == TitleState::Visible)
            visibleTimer = 1.5f;

        if (cur == TitleState::Visible && visibleTimer >= 0.0f) {
            visibleTimer -= dt;
            if (visibleTimer < 0.0f)
                title.TriggerOut();
        }

        if (prevState == TitleState::AnimatingOut && cur == TitleState::Hidden)
            hiddenTimer = 0.6f;

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
