// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "visual_element.h"
#include "render_util.h"

#include <algorithm>

// ── Data-change animation ─────────────────────────────────────────────────────

void VisualElement::SetContent(const std::string& value)
{
    if (m_contentEverSet && value == m_currentContent) return;

    if (dataOutAnimation.type != AnimationType::None && m_contentEverSet) {
        m_pendingContent   = value;
        m_dataAnimState    = DataAnimState::AnimatingOut;
        m_dataAnimTimer    = 0.0;
    } else {
        m_currentContent = value;
        ApplyContent(value);
        m_contentEverSet = true;
        if (dataInAnimation.type != AnimationType::None) {
            m_dataAnimState  = DataAnimState::AnimatingIn;
            m_dataAnimTimer  = 0.0;
        }
    }
}

void VisualElement::TickData(float dt)
{
    if (m_dataAnimState == DataAnimState::Idle) return;
    m_dataAnimTimer += dt;

    if (m_dataAnimState == DataAnimState::AnimatingOut) {
        float total = dataOutAnimation.delay + dataOutAnimation.duration;
        if (m_dataAnimTimer >= total) {
            m_currentContent = m_pendingContent;
            ApplyContent(m_pendingContent);
            m_contentEverSet = true;
            m_pendingContent.clear();
            m_dataAnimTimer = 0.0;
            m_dataAnimState = (dataInAnimation.type != AnimationType::None)
                                  ? DataAnimState::AnimatingIn
                                  : DataAnimState::Idle;
        }
    } else if (m_dataAnimState == DataAnimState::AnimatingIn) {
        float total = dataInAnimation.delay + dataInAnimation.duration;
        if (m_dataAnimTimer >= total)
            m_dataAnimState = DataAnimState::Idle;
    }
}

// ── Render ────────────────────────────────────────────────────────────────────

void VisualElement::Render(cairo_t* ctx, const AnimatedTransform& xf,
                           const AnimatedTransform* maskXf,
                           double timer, bool isOut,
                           double parentOffX, double parentOffY) const
{
    // Compose graphic-level xf with per-element data-change animation xf
    AnimatedTransform effectiveXf = xf;
    if (m_dataAnimState != DataAnimState::Idle) {
        bool dataIsOut = (m_dataAnimState == DataAnimState::AnimatingOut);
        const auto& dataDef = dataIsOut ? dataOutAnimation : dataInAnimation;
        AnimatedTransform dataXf = animation::EvaluateAnimation(
            dataDef, m_dataAnimTimer, dataIsOut, m_bounds.width, m_bounds.height);

        effectiveXf.opacity *= dataXf.opacity;
        effectiveXf.offsetX += dataXf.offsetX;
        effectiveXf.offsetY += dataXf.offsetY;
        effectiveXf.scale   *= dataXf.scale;
        // Data wipe overrides graphic wipe when active
        if (dataXf.clipW < 1.0 || dataXf.clipH < 1.0 ||
            dataXf.clipX > 0.0 || dataXf.clipY > 0.0) {
            effectiveXf.clipX = dataXf.clipX;
            effectiveXf.clipY = dataXf.clipY;
            effectiveXf.clipW = dataXf.clipW;
            effectiveXf.clipH = dataXf.clipH;
        }
    }

    if (opacity * effectiveXf.opacity < 1e-6f)
        return;

    cairo_translate(ctx, effectiveXf.offsetX, effectiveXf.offsetY);

    const bool isWiping = effectiveXf.clipX > 0.0 || effectiveXf.clipY > 0.0 ||
                          effectiveXf.clipW < 1.0  || effectiveXf.clipH < 1.0;

    if (shadow.enabled) {
        double sx, sy, sw, sh;
        if (isWiping) {
            sx = effectiveXf.clipX * m_bounds.width  + shadow.offsetX;
            sy = effectiveXf.clipY * m_bounds.height + shadow.offsetY;
            sw = effectiveXf.clipW * m_bounds.width;
            sh = effectiveXf.clipH * m_bounds.height;
        } else {
            sx = shadow.offsetX;
            sy = shadow.offsetY;
            sw = m_bounds.width;
            sh = m_bounds.height;
        }
        float cr = *std::min_element(cornerRadius, cornerRadius + 4);
        cairo_save(ctx);
        render::RenderDropShadow(ctx, sx, sy, sw, sh, shadow.blur,
                                 shadow.color[0], shadow.color[1], shadow.color[2],
                                 shadow.color[3] * (double)(opacity * effectiveXf.opacity), cr);
        cairo_restore(ctx);
    }

    if (isWiping) {
        cairo_rectangle(ctx, 0, 0, m_bounds.width, m_bounds.height);
        cairo_clip(ctx);
        cairo_rectangle(ctx, effectiveXf.clipX * m_bounds.width, effectiveXf.clipY * m_bounds.height,
                        effectiveXf.clipW * m_bounds.width, effectiveXf.clipH * m_bounds.height);
        cairo_clip(ctx);
    }

    double cx = m_bounds.width / 2.0;
    double cy = m_bounds.height / 2.0;

    const bool useGroup = !m_children.empty() || (double)opacity * effectiveXf.opacity < 1.0;
    if (useGroup)
        cairo_push_group(ctx);

    cairo_save(ctx);

    if (m_shearX != 0.0f || m_shearY != 0.0f) {
        cairo_translate(ctx, cx, cy);
        cairo_matrix_t shearMat;
        cairo_matrix_init(&shearMat, 1.0, (double)m_shearY, (double)m_shearX, 1.0, 0.0, 0.0);
        cairo_transform(ctx, &shearMat);
        cairo_translate(ctx, -cx, -cy);
    }

    ApplyClipping(ctx, effectiveXf, maskXf, parentOffX, parentOffY);

    if (effectiveXf.scale != 1.0 && effectiveXf.scale > 1e-6) {
        cairo_translate(ctx, cx, cy);
        cairo_scale(ctx, effectiveXf.scale, effectiveXf.scale);
        cairo_translate(ctx, -cx, -cy);
    }

    if (m_rotation != 0.0f) {
        cairo_translate(ctx, cx, cy);
        cairo_rotate(ctx, m_rotation * Pi / 180.0);
        cairo_translate(ctx, -cx, -cy);
    }

    RenderContent(ctx);

    cairo_restore(ctx);

    if (!m_children.empty()) {
        std::vector<VisualElement*> sorted;
        sorted.reserve(m_children.size());
        for (IElement* child : m_children) {
            if (auto* ve = dynamic_cast<VisualElement*>(child))
                sorted.push_back(ve);
        }
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](VisualElement* a, VisualElement* b) { return a->zOrder < b->zOrder; });

        cairo_save(ctx);
        for (VisualElement* child : sorted) {
            const auto& def = isOut ? child->outAnimation : child->inAnimation;
            AnimatedTransform childXf = animation::EvaluateAnimation(
                def, timer, isOut, child->GetSize().width, child->GetSize().height);

            const AnimatedTransform* childMaskXf = nullptr;
            AnimatedTransform childMaskXfVal;
            if (child->mask) {
                const auto& mDef = isOut ? child->mask->outAnimation : child->mask->inAnimation;
                childMaskXfVal = animation::EvaluateAnimation(
                    mDef, timer, isOut,
                    child->mask->GetSize().width, child->mask->GetSize().height);
                childMaskXf = &childMaskXfVal;
            }

            cairo_save(ctx);
            cairo_translate(ctx, child->GetPosition().x, child->GetPosition().y);
            child->Render(ctx, childXf, childMaskXf, timer, isOut,
                          parentOffX + effectiveXf.offsetX, parentOffY + effectiveXf.offsetY);
            cairo_restore(ctx);
        }
        cairo_restore(ctx);
    }

    if (useGroup) {
        cairo_pop_group_to_source(ctx);
        cairo_paint_with_alpha(ctx, opacity * effectiveXf.opacity);
    }
}

void VisualElement::ApplyClipping(cairo_t* ctx, const AnimatedTransform& xf,
                                   const AnimatedTransform* maskXf,
                                   double parentOffX, double parentOffY) const
{
    const bool isWiping = xf.clipX > 0.0 || xf.clipY > 0.0 || xf.clipW < 1.0 || xf.clipH < 1.0;
    const bool hasRadius = cornerRadius[0] > 0 || cornerRadius[1] > 0 ||
                           cornerRadius[2] > 0 || cornerRadius[3] > 0;

    if (isWiping || hasRadius) {
        render::RoundRect(ctx, 0, 0, m_bounds.width, m_bounds.height, cornerRadius);
        cairo_clip(ctx);
    }

    if (mask != nullptr) {
        Point maskGlobal = mask->GetGlobalPosition();
        Point selfGlobal = GetGlobalPosition();
        float mx = (float)(maskGlobal.x - selfGlobal.x) - (float)xf.offsetX - (float)parentOffX;
        float my = (float)(maskGlobal.y - selfGlobal.y) - (float)xf.offsetY - (float)parentOffY;
        if (maskXf) {
            mx += (float)maskXf->offsetX;
            my += (float)maskXf->offsetY;
        }
        const auto& mb = mask->GetBounds();
        render::RoundRect(ctx, mx, my, mb.width, mb.height, mask->cornerRadius);
        cairo_clip(ctx);
    }

    if (isWiping) {
        cairo_rectangle(ctx, xf.clipX * m_bounds.width, xf.clipY * m_bounds.height,
                        xf.clipW * m_bounds.width, xf.clipH * m_bounds.height);
        cairo_clip(ctx);
    }
}

void VisualElement::ApplyFillAndStroke(cairo_t* ctx) const
{
    bool hasFill   = fill.Valid();
    bool hasStroke = stroke.Valid() && strokeWidth > 0.0f;

    if (hasFill) {
        fill.Apply(ctx, m_bounds.width, m_bounds.height);
        if (hasStroke) cairo_fill_preserve(ctx);
        else           cairo_fill(ctx);
    }

    if (hasStroke) {
        stroke.Apply(ctx, m_bounds.width, m_bounds.height);
        cairo_set_line_width(ctx, strokeWidth);
        cairo_stroke(ctx);
    }
}
