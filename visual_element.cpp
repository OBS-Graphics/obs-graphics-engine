// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "visual_element.h"
#include "render_util.h"

#include <algorithm>
#include <cstring>

// ── Data-change animation ─────────────────────────────────────────────────────

void VisualElement::SetContent(const std::string& value)
{
    if (m_contentEverSet && value == m_currentContent) return;
    if (m_dataAnimState == DataAnimState::AnimatingOut && value == m_pendingContent) return;

    if (dataOutAnimation.type != AnimationType::None && m_contentEverSet) {
        m_pendingContent   = value;
        m_dataAnimState    = DataAnimState::AnimatingOut;
        m_dataAnimTimer    = 0.0;
    } else {
        m_currentContent = value;
        ApplyContent(value);
        if (m_contentEverSet && dataInAnimation.type != AnimationType::None) {
            m_dataAnimState  = DataAnimState::AnimatingIn;
            m_dataAnimTimer  = 0.0;
        }
        m_contentEverSet = true;
    }
}

void VisualElement::SetDataPreviewTime(bool isOut, double t)
{
    m_dataAnimState = isOut ? DataAnimState::AnimatingOut : DataAnimState::AnimatingIn;
    m_dataAnimTimer = t;
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

// ── Internal helpers ──────────────────────────────────────────────────────────

// Clips ctx to the wipe rect defined in the element's sheared coordinate space.
// cairo_set_matrix restores the transform while leaving the clip baked in device coords.
static void ApplyShearAwareWipeClip(cairo_t* ctx, const Transform& tf,
                                     double cx, double cy,
                                     double clipX, double clipY, double clipW, double clipH,
                                     double boundsW, double boundsH)
{
    cairo_matrix_t mat;
    cairo_get_matrix(ctx, &mat);
    tf.Apply(ctx, cx, cy);
    cairo_rectangle(ctx, 0.0, 0.0, boundsW, boundsH);
    cairo_clip(ctx);
    cairo_rectangle(ctx, clipX, clipY, clipW, clipH);
    cairo_clip(ctx);
    cairo_set_matrix(ctx, &mat);
}

// ── Private render helpers ────────────────────────────────────────────────────

AnimatedTransform VisualElement::ComposeDataAnimation(const AnimatedTransform& xf) const
{
    if (m_dataAnimState == DataAnimState::Idle)
        return xf;

    bool dataIsOut = (m_dataAnimState == DataAnimState::AnimatingOut);
    const auto& dataDef = dataIsOut ? dataOutAnimation : dataInAnimation;
    AnimatedTransform dataXf = animation::EvaluateAnimation(
        dataDef, m_dataAnimTimer, dataIsOut, m_bounds.width, m_bounds.height);

    AnimatedTransform result = xf;
    result.opacity *= dataXf.opacity;
    result.offsetX += dataXf.offsetX;
    result.offsetY += dataXf.offsetY;
    result.scale   *= dataXf.scale;
    if (dataXf.clipW < 1.0 || dataXf.clipH < 1.0 ||
        dataXf.clipX > 0.0 || dataXf.clipY > 0.0) {
        result.clipX = dataXf.clipX;
        result.clipY = dataXf.clipY;
        result.clipW = dataXf.clipW;
        result.clipH = dataXf.clipH;
    }
    return result;
}

void VisualElement::RenderShadow(cairo_t* ctx, const AnimatedTransform& xf,
                                  const AnimatedTransform* maskXf,
                                  double parentOffX, double parentOffY) const
{
    if (!shadow.enabled) return;

    const double alpha = shadow.color[3] * (double)(opacity * xf.opacity);
    const int sw = (int)std::ceil(m_bounds.width);
    const int sh = (int)std::ceil(m_bounds.height);
    if (sw <= 0 || sh <= 0) return;

    const Transform tf = GetTransform();

    // Wipe region in element-local coords. Default (no wipe) = full bounds.
    const double wipeX = xf.clipX * m_bounds.width;
    const double wipeY = xf.clipY * m_bounds.height;
    const double wipeW = xf.clipW * m_bounds.width;
    const double wipeH = xf.clipH * m_bounds.height;

    const bool isWiping = xf.clipX > 0.0 || xf.clipY > 0.0 ||
                          xf.clipW < 1.0  || xf.clipH < 1.0;

    cairo_save(ctx);
    ApplyMaskClip(ctx, xf, maskXf, parentOffX, parentOffY);

    cairo_surface_t* customSurf = CreateShadowSurface(sw, sh);
    if (customSurf) {
        // Zero out pixels outside the wipe region so only the revealed portion
        // of the glyph outlines contributes to the blur.
        if (isWiping) {
            cairo_surface_flush(customSurf);
            uint8_t*   pix    = cairo_image_surface_get_data(customSurf);
            const int  stride = cairo_image_surface_get_stride(customSurf);
            const int  surfW  = cairo_image_surface_get_width(customSurf);
            const int  surfH  = cairo_image_surface_get_height(customSurf);
            const int  x0 = (int)std::floor(wipeX);
            const int  y0 = (int)std::floor(wipeY);
            const int  x1 = std::min((int)std::ceil(wipeX + wipeW), surfW);
            const int  y1 = std::min((int)std::ceil(wipeY + wipeH), surfH);
            for (int y = 0; y < surfH; ++y) {
                uint8_t* row = pix + (size_t)y * stride;
                if (y < y0 || y >= y1) { std::memset(row, 0, surfW); continue; }
                if (x0 > 0)     std::memset(row,       0, x0);
                if (x1 < surfW) std::memset(row + x1,  0, surfW - x1);
            }
            cairo_surface_mark_dirty(customSurf);
        }
        render::RenderDropShadowFromSurface(ctx, customSurf,
            shadow.offsetX, shadow.offsetY, shadow.blur,
            shadow.color[0], shadow.color[1], shadow.color[2], alpha, tf);
        cairo_surface_destroy(customSurf);
    } else {
        float cr = *std::min_element(cornerRadius, cornerRadius + 4);
        render::RenderDropShadow(ctx,
            shadow.offsetX + wipeX, shadow.offsetY + wipeY,
            wipeW, wipeH, shadow.blur,
            shadow.color[0], shadow.color[1], shadow.color[2], alpha, cr, tf);
    }
    cairo_restore(ctx);
}

void VisualElement::RenderChildren(cairo_t* ctx, const AnimatedTransform& effectiveXf,
                                    double timer, bool isOut,
                                    double accParentOffX, double accParentOffY) const
{
    if (m_children.empty()) return;

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
        child->Render(ctx, childXf, childMaskXf, timer, isOut, accParentOffX, accParentOffY);
        cairo_restore(ctx);
    }
    cairo_restore(ctx);
}

// ── Render ────────────────────────────────────────────────────────────────────

void VisualElement::Render(cairo_t* ctx, const AnimatedTransform& xf,
                           const AnimatedTransform* maskXf,
                           double timer, bool isOut,
                           double parentOffX, double parentOffY) const
{
    const AnimatedTransform effectiveXf = ComposeDataAnimation(xf);

    if (opacity * effectiveXf.opacity < 1e-6f)
        return;

    cairo_translate(ctx, effectiveXf.offsetX, effectiveXf.offsetY);

    RenderShadow(ctx, effectiveXf, maskXf, parentOffX, parentOffY);

    const bool isWiping = effectiveXf.clipX > 0.0 || effectiveXf.clipY > 0.0 ||
                          effectiveXf.clipW < 1.0  || effectiveXf.clipH < 1.0;
    if (isWiping) {
        const double cx = m_bounds.width / 2.0;
        const double cy = m_bounds.height / 2.0;
        ApplyShearAwareWipeClip(ctx, GetTransform(), cx, cy,
            effectiveXf.clipX * m_bounds.width, effectiveXf.clipY * m_bounds.height,
            effectiveXf.clipW * m_bounds.width, effectiveXf.clipH * m_bounds.height,
            m_bounds.width, m_bounds.height);
    }

    const double cx = m_bounds.width / 2.0;
    const double cy = m_bounds.height / 2.0;

    const bool useGroup = !m_children.empty() || (double)opacity * effectiveXf.opacity < 1.0;
    if (useGroup)
        cairo_push_group(ctx);

    cairo_save(ctx);
    // Mask clip precedes the element transform: the mask lives in world space and
    // must not be distorted by this element's shear or rotation.
    ApplyMaskClip(ctx, effectiveXf, maskXf, parentOffX, parentOffY);
    GetTransform().Apply(ctx, cx, cy);   // shear, then rotation
    ApplyClipping(ctx, effectiveXf);     // corner-radius + wipe (in fully-transformed space)
    if (effectiveXf.scale != 1.0 && effectiveXf.scale > 1e-6) {
        cairo_translate(ctx, cx, cy);
        cairo_scale(ctx, effectiveXf.scale, effectiveXf.scale);
        cairo_translate(ctx, -cx, -cy);
    }
    RenderContent(ctx);
    cairo_restore(ctx);

    RenderChildren(ctx, effectiveXf, timer, isOut,
                   parentOffX + effectiveXf.offsetX, parentOffY + effectiveXf.offsetY);

    if (useGroup) {
        cairo_pop_group_to_source(ctx);
        cairo_paint_with_alpha(ctx, opacity * effectiveXf.opacity);
    }
}

// ── Clipping ──────────────────────────────────────────────────────────────────

void VisualElement::ApplyMaskClip(cairo_t* ctx, const AnimatedTransform& xf,
                                   const AnimatedTransform* maskXf,
                                   double parentOffX, double parentOffY) const
{
    if (mask == nullptr) return;
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

void VisualElement::ApplyClipping(cairo_t* ctx, const AnimatedTransform& xf) const
{
    const bool isWiping = xf.clipX > 0.0 || xf.clipY > 0.0 || xf.clipW < 1.0 || xf.clipH < 1.0;
    const bool hasRadius = cornerRadius[0] > 0 || cornerRadius[1] > 0 ||
                           cornerRadius[2] > 0 || cornerRadius[3] > 0;

    if (isWiping || hasRadius) {
        render::RoundRect(ctx, 0, 0, m_bounds.width, m_bounds.height, cornerRadius);
        cairo_clip(ctx);
    }

    if (isWiping) {
        cairo_rectangle(ctx, xf.clipX * m_bounds.width, xf.clipY * m_bounds.height,
                        xf.clipW * m_bounds.width, xf.clipH * m_bounds.height);
        cairo_clip(ctx);
    }
}

// ── Fill & stroke ─────────────────────────────────────────────────────────────

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
