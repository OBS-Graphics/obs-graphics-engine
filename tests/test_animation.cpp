// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Visual test: in/out animation cycle with wipe and slide effects.
// A lower-third graphic animates in via WipeRight, holds for 2.5s, then
// animates out via WipeLeft. The cycle repeats automatically.
//
// Tests: wipe clip, staggered delays, text shadow, state machine.

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

    // Background bar with gradient fill
    {
        auto el = std::make_unique<RectangleElement>();
        el->SetId("bar");
        el->SetBounds({80.0, 590.0, 960.0, 88.0});
        auto fill = Paint::Linear(0.0, 0.0, 1.0, 0.0);
        fill.AddStop(0.0, 0.07, 0.12, 0.30);
        fill.AddStop(1.0, 0.11, 0.20, 0.48);
        el->fill = fill;
        for (int i = 0; i < 4; ++i) el->cornerRadius[i] = 5.0f;
        el->inAnimation  = {AnimationType::WipeRight, Easing::EaseOutCubic, 0.70f, 0.00f};
        el->outAnimation = {AnimationType::WipeLeft,  Easing::EaseInCubic,  0.55f, 0.00f};
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    // Accent stripe on the left edge of the bar
    {
        auto el = std::make_unique<RectangleElement>();
        el->SetId("stripe");
        el->SetBounds({80.0, 590.0, 6.0, 88.0});
        el->fill = Paint::Solid(0.22, 0.57, 0.92);
        for (int i = 0; i < 4; ++i) el->cornerRadius[i] = 5.0f;
        el->inAnimation  = {AnimationType::WipeRight, Easing::EaseOutCubic, 0.70f, 0.06f};
        el->outAnimation = {AnimationType::WipeLeft,  Easing::EaseInCubic,  0.55f, 0.00f};
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    // Name text
    {
        auto el = std::make_unique<TextElement>();
        el->SetId("name");
        el->SetBounds({104.0, 598.0, 820.0, 42.0});
        el->text        = "John Smith";
        el->font.family = "Sans";
        el->font.size   = 30.0f;
        el->font.weight = FontWeight::Bold;
        el->fill        = Paint::Solid(1.0, 1.0, 1.0);
        el->shadow.enabled  = true;
        el->shadow.blur     = 5.0;
        el->shadow.offsetX  = 1.0;
        el->shadow.offsetY  = 1.5;
        el->shadow.color[3] = 0.45f;
        el->inAnimation  = {AnimationType::WipeRight, Easing::EaseOutCubic, 0.70f, 0.12f};
        el->outAnimation = {AnimationType::WipeLeft,  Easing::EaseInCubic,  0.55f, 0.00f};
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    // Role text
    {
        auto el = std::make_unique<TextElement>();
        el->SetId("role");
        el->SetBounds({104.0, 643.0, 820.0, 28.0});
        el->text        = "Software Engineer";
        el->font.family = "Sans";
        el->font.size   = 20.0f;
        el->fill        = Paint::Solid(0.58, 0.78, 0.96);
        el->inAnimation  = {AnimationType::WipeRight, Easing::EaseOutCubic, 0.70f, 0.22f};
        el->outAnimation = {AnimationType::WipeLeft,  Easing::EaseInCubic,  0.55f, 0.00f};
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    return t;
}

} // namespace

int main()
{
    SDLRunner app(1280, 720, "Animation Test");
    Title title = buildTitle();
    title.TriggerIn();

    TitleState prevState = TitleState::Hidden;
    float visibleTimer = -1.0f;  // countdown while Visible before TriggerOut
    float hiddenTimer  = -1.0f;  // countdown while Hidden before next TriggerIn

    app.run([&](cairo_t* cr, float dt) {
        title.Tick(dt);
        TitleState cur = title.state;

        // Visible: start a 2.5s hold before animating out
        if (prevState == TitleState::AnimatingIn && cur == TitleState::Visible)
            visibleTimer = 2.5f;

        if (cur == TitleState::Visible && visibleTimer >= 0.0f) {
            visibleTimer -= dt;
            if (visibleTimer < 0.0f)
                title.TriggerOut();
        }

        // Hidden: wait 0.8s then loop
        if (prevState == TitleState::AnimatingOut && cur == TitleState::Hidden)
            hiddenTimer = 0.8f;

        if (hiddenTimer >= 0.0f) {
            hiddenTimer -= dt;
            if (hiddenTimer < 0.0f) {
                title.TriggerIn();
                hiddenTimer = -1.0f;
            }
        }

        prevState = cur;

        // Background
        cairo_set_source_rgb(cr, 0.08, 0.09, 0.14);
        cairo_paint(cr);

        // Simulated video content (placeholder grid)
        cairo_set_source_rgb(cr, 0.14, 0.15, 0.21);
        for (int y = 0; y < 720; y += 120)
            for (int x = 0; x < 1280; x += 160) {
                cairo_rectangle(cr, x + 2, y + 2, 156, 116);
                cairo_fill(cr);
            }

        title.Render(cr);
    });
}
