// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Visual test: data-change animation.
// A lower-third stays Visible. Clicking name/role buttons calls SetContent(),
// triggering the per-element dataIn/Out animation cycle without hiding the
// graphic. The "Data Anim" row switches which AnimationType drives that cycle.
// Trigger In / Trigger Out play the graphic's own entry/exit animation.

#include "title.h"
#include "element_rectangle.h"
#include "element_text.h"
#include "sdl_runner.h"

#include <SDL3/SDL.h>
#include <cairo/cairo.h>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

// ── Button ────────────────────────────────────────────────────────────────────

struct Button {
    double x, y, w, h;
    std::string label;
    std::function<void()> action;
    bool* active{nullptr};
};

static bool hitTest(const Button& b, float mx, float my)
{
    return mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h;
}

static void drawButton(cairo_t* cr, const Button& b, bool hover)
{
    bool on = b.active && *b.active;

    if (on)
        cairo_set_source_rgba(cr, 0.22, 0.57, 0.92, 1.0);
    else if (hover)
        cairo_set_source_rgba(cr, 0.25, 0.27, 0.35, 1.0);
    else
        cairo_set_source_rgba(cr, 0.16, 0.18, 0.25, 1.0);

    constexpr double r = 5.0;
    double x = b.x, y = b.y, w = b.w, h = b.h;
    cairo_new_path(cr);
    cairo_arc(cr, x+r,   y+r,   r, M_PI,       3*M_PI/2);
    cairo_arc(cr, x+w-r, y+r,   r, 3*M_PI/2,   0);
    cairo_arc(cr, x+w-r, y+h-r, r, 0,           M_PI/2);
    cairo_arc(cr, x+r,   y+h-r, r, M_PI/2,      M_PI);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, on ? 1.0 : 0.82, on ? 1.0 : 0.82, on ? 1.0 : 0.88, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.5);
    cairo_text_extents_t te;
    cairo_text_extents(cr, b.label.c_str(), &te);
    cairo_move_to(cr, x + w/2.0 - te.width/2.0 - te.x_bearing,
                      y + h/2.0 - te.height/2.0 - te.y_bearing);
    cairo_show_text(cr, b.label.c_str());
}

static void drawRowLabel(cairo_t* cr, const char* text, double x, double y, double h)
{
    cairo_set_source_rgba(cr, 0.42, 0.44, 0.54, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);
    cairo_text_extents_t te;
    cairo_text_extents(cr, text, &te);
    cairo_move_to(cr, x, y + h/2.0 - te.height/2.0 - te.y_bearing);
    cairo_show_text(cr, text);
}

// ── Title construction ────────────────────────────────────────────────────────

Title buildTitle(TextElement*& nameOut, TextElement*& roleOut)
{
    Title t;
    t.width  = 1280;
    t.height = 720;

    {
        auto el = std::make_unique<RectangleElement>();
        el->SetId("bar");
        el->SetBounds({80.0, 590.0, 960.0, 90.0});
        auto fill = Paint::Linear(0.0, 0.0, 1.0, 0.0);
        fill.AddStop(0.0, 0.07, 0.12, 0.30);
        fill.AddStop(1.0, 0.11, 0.20, 0.48);
        el->fill = fill;
        for (int i = 0; i < 4; ++i) el->cornerRadius[i] = 6.0f;
        el->inAnimation  = {AnimationType::WipeRight, Easing::EaseOutCubic, 0.60f, 0.00f};
        el->outAnimation = {AnimationType::WipeLeft,  Easing::EaseInCubic,  0.50f, 0.00f};
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    {
        auto el = std::make_unique<RectangleElement>();
        el->SetId("stripe");
        el->SetBounds({80.0, 590.0, 6.0, 90.0});
        el->fill = Paint::Solid(0.22, 0.57, 0.92);
        for (int i = 0; i < 4; ++i) el->cornerRadius[i] = 6.0f;
        el->inAnimation  = {AnimationType::WipeRight, Easing::EaseOutCubic, 0.60f, 0.07f};
        el->outAnimation = {AnimationType::WipeLeft,  Easing::EaseInCubic,  0.50f, 0.00f};
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

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
        el->inAnimation  = {AnimationType::WipeRight, Easing::EaseOutCubic, 0.60f, 0.12f};
        el->outAnimation = {AnimationType::WipeLeft,  Easing::EaseInCubic,  0.50f, 0.00f};
        el->dataInAnimation  = {AnimationType::SlideUp, Easing::EaseOutCubic, 0.35f, 0.00f};
        el->dataOutAnimation = {AnimationType::SlideUp, Easing::EaseInCubic,  0.25f, 0.00f};
        nameOut = el.get();
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    {
        auto el = std::make_unique<TextElement>();
        el->SetId("role");
        el->SetBounds({104.0, 643.0, 820.0, 28.0});
        el->text        = "Software Engineer";
        el->font.family = "Sans";
        el->font.size   = 20.0f;
        el->fill        = Paint::Solid(0.58, 0.78, 0.96);
        el->inAnimation  = {AnimationType::WipeRight, Easing::EaseOutCubic, 0.60f, 0.22f};
        el->outAnimation = {AnimationType::WipeLeft,  Easing::EaseInCubic,  0.50f, 0.00f};
        el->dataInAnimation  = {AnimationType::Fade,  Easing::EaseOut,      0.30f, 0.05f};
        el->dataOutAnimation = {AnimationType::Fade,  Easing::EaseIn,       0.20f, 0.00f};
        roleOut = el.get();
        t.GetRoot()->AddChild(el.get());
        t.elements.push_back(std::move(el));
    }

    return t;
}

} // namespace

int main()
{
    SDLRunner app(1280, 720, "Data Change Animation Test");

    TextElement* nameEl = nullptr;
    TextElement* roleEl = nullptr;
    Title title = buildTitle(nameEl, roleEl);
    title.TriggerIn();

    // ── Preset state ──────────────────────────────────────────────────────────

    struct Preset { std::string label; bool active; };

    std::vector<Preset> names = {
        {"John Smith",    true },
        {"Maria Garcia",  false},
        {"Alex Kim",      false},
    };
    std::vector<Preset> roles = {
        {"Software Engineer", true },
        {"Art Director",      false},
        {"Producer",          false},
    };
    struct AnimChoice { std::string label; AnimationType type; bool active; };
    std::vector<AnimChoice> animChoices = {
        {"Fade",      AnimationType::Fade,      false},
        {"SlideUp",   AnimationType::SlideUp,   true },
        {"WipeRight", AnimationType::WipeRight, false},
        {"ScaleIn",   AnimationType::ScaleIn,   false},
    };

    auto setName = [&](int i) {
        for (auto& p : names) p.active = false;
        names[i].active = true;
        nameEl->SetContent(names[i].label);
    };
    auto setRole = [&](int i) {
        for (auto& p : roles) p.active = false;
        roles[i].active = true;
        roleEl->SetContent(roles[i].label);
    };
    auto setDataAnim = [&](int i) {
        for (auto& c : animChoices) c.active = false;
        animChoices[i].active = true;
        AnimationType t = animChoices[i].type;
        nameEl->dataInAnimation  = {t, Easing::EaseOutCubic, 0.35f, 0.00f};
        nameEl->dataOutAnimation = {t, Easing::EaseInCubic,  0.25f, 0.00f};
        roleEl->dataInAnimation  = {t, Easing::EaseOut,      0.30f, 0.05f};
        roleEl->dataOutAnimation = {t, Easing::EaseIn,       0.20f, 0.00f};
    };

    // ── Build buttons ─────────────────────────────────────────────────────────

    constexpr double kBtnH    = 30.0;
    constexpr double kGap     = 7.0;
    constexpr double kLabelW  = 82.0;
    constexpr double kLeft    = 16.0;
    constexpr double kRows[4] = {16.0, 58.0, 100.0, 142.0};

    std::vector<Button> buttons;

    // Row 0: graphic control
    buttons.push_back({kLeft + kLabelW,       kRows[0], 108, kBtnH, "Trigger In",
                       [&]{ title.TriggerIn(); },  nullptr});
    buttons.push_back({kLeft + kLabelW + 115, kRows[0], 108, kBtnH, "Trigger Out",
                       [&]{ title.TriggerOut(); }, nullptr});

    // Row 1: name presets
    {
        double x = kLeft + kLabelW;
        for (int i = 0; i < (int)names.size(); ++i) {
            double bw = 125.0;
            buttons.push_back({x, kRows[1], bw, kBtnH, names[i].label,
                               [&, i]{ setName(i); }, &names[i].active});
            x += bw + kGap;
        }
    }

    // Row 2: role presets
    {
        double x = kLeft + kLabelW;
        for (int i = 0; i < (int)roles.size(); ++i) {
            double bw = 140.0;
            buttons.push_back({x, kRows[2], bw, kBtnH, roles[i].label,
                               [&, i]{ setRole(i); }, &roles[i].active});
            x += bw + kGap;
        }
    }

    // Row 3: data anim type
    {
        double x = kLeft + kLabelW;
        for (int i = 0; i < (int)animChoices.size(); ++i) {
            double bw = 95.0;
            buttons.push_back({x, kRows[3], bw, kBtnH, animChoices[i].label,
                               [&, i]{ setDataAnim(i); }, &animChoices[i].active});
            x += bw + kGap;
        }
    }

    // ── Event + render loop ───────────────────────────────────────────────────

    Uint64 prev = SDL_GetTicks();
    bool running = true;

    while (running) {
        float mx = 0.0f, my = 0.0f;
        SDL_GetMouseState(&mx, &my);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { running = false; break; }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) { running = false; break; }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                for (auto& btn : buttons) {
                    if (hitTest(btn, e.button.x, e.button.y)) {
                        btn.action();
                        break;
                    }
                }
            }
        }
        if (!running) break;

        Uint64 now = SDL_GetTicks();
        float dt = static_cast<float>(now - prev) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        prev = now;

        title.Tick(dt);

        cairo_t* cr = app.cr;

        // Background
        cairo_set_source_rgb(cr, 0.08, 0.09, 0.13);
        cairo_paint(cr);

        // Control panel background
        cairo_set_source_rgba(cr, 0.11, 0.12, 0.18, 1.0);
        cairo_rectangle(cr, 0, 0, 1280, 188);
        cairo_fill(cr);

        // Separator line
        cairo_set_source_rgba(cr, 0.20, 0.21, 0.30, 1.0);
        cairo_rectangle(cr, 0, 187, 1280, 1);
        cairo_fill(cr);

        // Row labels
        drawRowLabel(cr, "Graphic:",  kLeft, kRows[0], kBtnH);
        drawRowLabel(cr, "Name:",     kLeft, kRows[1], kBtnH);
        drawRowLabel(cr, "Role:",     kLeft, kRows[2], kBtnH);
        drawRowLabel(cr, "Data Anim:", kLeft, kRows[3], kBtnH);

        // Buttons
        for (auto& btn : buttons)
            drawButton(cr, btn, hitTest(btn, mx, my));

        // State badge (top-right)
        {
            const char* stateStr = "Hidden";
            double sr = 0.45, sg = 0.47, sb = 0.55;
            switch (title.state) {
            case TitleState::AnimatingIn:  stateStr = "Animating In";  sr=0.3; sg=0.75; sb=0.4; break;
            case TitleState::Visible:      stateStr = "Visible";       sr=0.22; sg=0.57; sb=0.92; break;
            case TitleState::AnimatingOut: stateStr = "Animating Out"; sr=0.85; sg=0.60; sb=0.2; break;
            default: break;
            }
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 11.0);
            cairo_text_extents_t te;
            cairo_text_extents(cr, stateStr, &te);
            double bx = 1280.0 - te.width - 28.0, by = 10.0, bw = te.width + 18.0, bh = 22.0;
            cairo_set_source_rgba(cr, sr*0.2, sg*0.2, sb*0.2, 0.9);
            cairo_rectangle(cr, bx, by, bw, bh);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, sr, sg, sb, 1.0);
            cairo_move_to(cr, bx + 9.0, by + bh/2.0 - te.height/2.0 - te.y_bearing);
            cairo_show_text(cr, stateStr);
        }

        // Simulated video grid below the panel
        cairo_set_source_rgb(cr, 0.11, 0.12, 0.18);
        for (int y = 195; y < 720; y += 100)
            for (int x = 0; x < 1280; x += 130) {
                cairo_rectangle(cr, x + 2, y + 2, 126, 96);
                cairo_fill(cr);
            }

        title.Render(cr);
        app.flush();
    }
}
