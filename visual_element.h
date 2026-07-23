// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "animation.h"
#include "spatial.h"
#include "types.hpp"

#include <nlohmann/json.hpp>

class VisualElement : public Spatial {
public:
    int zOrder{0};

    // Element-level JSON keys the parser doesn't recognize (e.g. fields
    // written by a newer plugin/editor). Captured on parse, merged back in
    // (without overwriting known fields) on serialize, so a Load->Save
    // round-trip never silently drops forward-compat data. See
    // ParseElement/SerializeElement in title.cpp.
    nlohmann::json extra = nlohmann::json::object();

    AnimationDef inAnimation{}, outAnimation{};
    AnimationDef dataInAnimation{}, dataOutAnimation{};

    Paint fill, stroke;
    float strokeWidth{0.0f};
    float cornerRadius[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float opacity{1.0f};

    bool clipChildren{false};
    bool fitToChildren{false};
    float childrenPadding[4]{0.0f, 0.0f, 0.0f, 0.0f};

    struct DropShadow {
        bool enabled{false};
        double offsetX{4.0};
        double offsetY{4.0};
        double blur{8.0};
        float color[4]{0.0f, 0.0f, 0.0f, 0.8f};
    } shadow{};

    void Render(cairo_t* ctx, const AnimatedTransform& xf,
                double timer, bool isOut) const override;

    void ApplyClipping(cairo_t* ctx, const AnimatedTransform& xf) const override;

    // Triggers data-change animation; subclasses override ApplyContent instead.
    virtual void SetContent(const std::string& value) final;

    // Applies content immediately, resetting any in-flight data animation.
    // Use when the title is Hidden so no stale animation state bleeds into TriggerIn.
    void SetContentInstant(const std::string& value);

    // Called by Title::Tick() while Visible to advance data-change animation state.
    void TickData(float dt);

    // Directly sets data-animation state for scrub preview (bypasses content logic).
    void SetDataPreviewTime(bool isOut, double t);

protected:
    virtual void ApplyContent(const std::string& value) {}
    virtual void RenderContent(cairo_t* ctx) const = 0;
    void ApplyFillAndStroke(cairo_t* ctx) const;

    // Returns an A8 surface representing this element's shadow shape, or nullptr
    // to fall back to a rounded-rect shadow.
    virtual cairo_surface_t* CreateShadowSurface(int w, int h) const { return nullptr; }

private:
    enum class DataAnimState { Idle, AnimatingOut, AnimatingIn };
    mutable DataAnimState m_dataAnimState{DataAnimState::Idle};
    mutable double m_dataAnimTimer{0.0};
    mutable std::string m_currentContent;
    mutable std::string m_pendingContent;
    mutable bool m_contentEverSet{false};

    AnimatedTransform ComposeDataAnimation(const AnimatedTransform& xf) const;
    void RenderShadow(cairo_t* ctx, const AnimatedTransform& xf) const;
    void RenderChildren(cairo_t* ctx, const AnimatedTransform& effectiveXf,
                        double timer, bool isOut) const;
};
