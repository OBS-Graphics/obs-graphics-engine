// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "animation.h"
#include "spatial.h"
#include "types.hpp"

class VisualElement : public Spatial {
public:
    int zOrder{0};

    AnimationDef inAnimation{}, outAnimation{};
    AnimationDef dataInAnimation{}, dataOutAnimation{};

    Paint fill, stroke;
    float strokeWidth{0.0f};
    float cornerRadius[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float opacity{1.0f};

    VisualElement* mask{nullptr};

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
                const AnimatedTransform* maskXf,
                double timer, bool isOut,
                double parentOffX = 0.0, double parentOffY = 0.0) const override;

    void ApplyClipping(cairo_t* ctx, const AnimatedTransform& xf,
                       const AnimatedTransform* maskXf,
                       double parentOffX, double parentOffY) const override;

    // Triggers data-change animation; subclasses override ApplyContent instead.
    virtual void SetContent(const std::string& value) final;

    // Called by Title::Tick() while Visible to advance data-change animation state.
    void TickData(float dt);

protected:
    virtual void ApplyContent(const std::string& value) {}
    virtual void RenderContent(cairo_t* ctx) const = 0;
    void ApplyFillAndStroke(cairo_t* ctx) const;

private:
    enum class DataAnimState { Idle, AnimatingOut, AnimatingIn };
    mutable DataAnimState m_dataAnimState{DataAnimState::Idle};
    mutable double m_dataAnimTimer{0.0};
    mutable std::string m_currentContent;
    mutable std::string m_pendingContent;
    mutable bool m_contentEverSet{false};
};
