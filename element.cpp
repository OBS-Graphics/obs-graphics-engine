#include "element.h"
#include "animation.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include "stb_image.h"

static std::shared_ptr<cairo_surface_t> LoadImageSurface(const std::string& path)
{
    int w, h, channels;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels)
        return {};

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_surface_flush(surface);

    uint8_t* dst = cairo_image_surface_get_data(surface);
    int dstStride = cairo_image_surface_get_stride(surface);

    for (int y = 0; y < h; y++) {
        const unsigned char* src = pixels + y * w * 4;
        uint32_t* row = reinterpret_cast<uint32_t*>(dst + y * dstStride);
        for (int x = 0; x < w; x++) {
            uint32_t r = src[0], g = src[1], b = src[2], a = src[3];
            // stb gives non-premultiplied RGBA; Cairo ARGB32 is premultiplied
            *row++ = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
            src += 4;
        }
    }

    cairo_surface_mark_dirty(surface);
    stbi_image_free(pixels);
    return std::shared_ptr<cairo_surface_t>(surface, cairo_surface_destroy);
}

// Drop shadow via mesh pattern: outer ring fades from shadowAlpha → transparent,
// inner area filled solid. cornerR matches element corner radius so shadow outline
// follows the element shape. 'a' is pre-baked with element opacity.
static void DrawDropShadow(cairo_t* ctx, double ox, double oy, double sw, double sh,
                           double blur, double r, double g, double b, double a,
                           float cornerR_in)
{
    if (a < 1e-6 || sw <= 0.0 || sh <= 0.0 || blur <= 0.0)
        return;

    float x = (float)ox, y = (float)oy;
    float w = (float)sw, h = (float)sh;
    float elevation = (float)blur;

    float cornerR = std::clamp(cornerR_in > 0.0f ? cornerR_in : 1.0f, 1.0f, std::min(w, h) / 2.0f);
    float edgeThickness = elevation * 2.0f;
    float inR = cornerR;
    float outR = cornerR + edgeThickness;

    const float e = 0.5f;
    float arg = std::clamp(2.0f * std::pow(1.0f - e / cornerR, 2.0f) - 1.0f, -1.0f, 1.0f);
    int stepsPerCorner = std::max(2, (int)std::ceil((float)(Pi / 2) / std::acos(arg)));

    struct Vec2 { float x, y; };
    Vec2 centers[4] = {
        {x + cornerR,     y + cornerR},
        {x + w - cornerR, y + cornerR},
        {x + w - cornerR, y + h - cornerR},
        {x + cornerR,     y + h - cornerR},
    };
    const float startAngles[4] = {(float)Pi, (float)(3 * Pi / 2), 0.0f, (float)(Pi / 2)};

    std::vector<Vec2> innerPts, outerPts;
    innerPts.reserve((stepsPerCorner + 1) * 4);
    outerPts.reserve((stepsPerCorner + 1) * 4);
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i <= stepsPerCorner; i++) {
            float angle = startAngles[c] + (float)i / stepsPerCorner * (float)(Pi / 2);
            innerPts.push_back({centers[c].x + inR * std::cos(angle),
                                centers[c].y + inR * std::sin(angle)});
            outerPts.push_back({centers[c].x + outR * std::cos(angle),
                                centers[c].y + outR * std::sin(angle)});
        }
    }

    cairo_pattern_t* pat = cairo_pattern_create_mesh();
    int n = (int)innerPts.size();

    // Gradient ring: inner edge = shadow colour, outer edge = transparent
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        cairo_mesh_pattern_begin_patch(pat);
        cairo_mesh_pattern_move_to(pat, innerPts[i].x, innerPts[i].y);
        cairo_mesh_pattern_line_to(pat, innerPts[j].x, innerPts[j].y);
        cairo_mesh_pattern_line_to(pat, outerPts[j].x, outerPts[j].y);
        cairo_mesh_pattern_line_to(pat, outerPts[i].x, outerPts[i].y);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 0, r, g, b, a);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 1, r, g, b, a);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 2, r, g, b, 0.0);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 3, r, g, b, 0.0);
        cairo_mesh_pattern_end_patch(pat);
    }

    // Solid fill: triangle fan from centre to inner ring
    float fcx = x + w * 0.5f, fcy = y + h * 0.5f;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        cairo_mesh_pattern_begin_patch(pat);
        cairo_mesh_pattern_move_to(pat, fcx, fcy);
        cairo_mesh_pattern_line_to(pat, innerPts[i].x, innerPts[i].y);
        cairo_mesh_pattern_line_to(pat, innerPts[j].x, innerPts[j].y);
        cairo_mesh_pattern_line_to(pat, fcx, fcy);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 0, r, g, b, a);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 1, r, g, b, a);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 2, r, g, b, a);
        cairo_mesh_pattern_set_corner_color_rgba(pat, 3, r, g, b, a);
        cairo_mesh_pattern_end_patch(pat);
    }

    cairo_set_source(ctx, pat);
    cairo_paint(ctx);
    cairo_pattern_destroy(pat);
}

static void RoundRect(cairo_t* ctx, double x, double y, double w, double h, const float r[4])
{
    double tl = r[0], tr = r[1], br = r[2], bl = r[3];
    cairo_new_path(ctx);
    cairo_arc(ctx, x + tl, y + tl, tl, Pi, 3 * Pi / 2);
    cairo_arc(ctx, x + w - tr, y + tr, tr, 3 * Pi / 2, 0);
    cairo_arc(ctx, x + w - br, y + h - br, br, 0, Pi / 2);
    cairo_arc(ctx, x + bl, y + h - bl, bl, Pi / 2, Pi);
    cairo_close_path(ctx);
}

void Element::Render(cairo_t* ctx, const AnimatedTransform& xf,
                     const AnimatedTransform* maskXf,
                     double timer, bool isOut,
                     double parentOffX, double parentOffY) const
{
    // Skip invisible elements — also prevents degenerate cairo_scale(0,0) for ScaleIn at t=0
    if (opacity * xf.opacity < 1e-6f)
        return;

    // Slide offset first so the clip region moves with the element, not against it
    cairo_translate(ctx, xf.offsetX, xf.offsetY);

    const bool isWiping = xf.clipX > 0.0 || xf.clipY > 0.0 || xf.clipW < 1.0 || xf.clipH < 1.0;

    // Shadow: rendered BEFORE any clip so it is never clipped by corner radius,
    // mask clipping, or the wipe rectangle. It tracks the wipe clip box when animating.
    if (shadow.enabled) {
        double sx, sy, sw, sh;
        if (isWiping) {
            sx = xf.clipX * bounds.width  + shadow.offsetX;
            sy = xf.clipY * bounds.height + shadow.offsetY;
            sw = xf.clipW * bounds.width;
            sh = xf.clipH * bounds.height;
        } else {
            sx = shadow.offsetX;
            sy = shadow.offsetY;
            sw = bounds.width;
            sh = bounds.height;
        }
        float cr = *std::min_element(cornerRadius, cornerRadius + 4);
        cairo_save(ctx);
        DrawDropShadow(ctx, sx, sy, sw, sh, (double)shadow.blur,
                       shadow.color[0], shadow.color[1], shadow.color[2],
                       shadow.color[3] * (double)(opacity * xf.opacity), cr);
        cairo_restore(ctx);
    }

    // Wipe clip AFTER shadow
    if (isWiping) {
        cairo_rectangle(ctx, 0, 0, bounds.width, bounds.height);
        cairo_clip(ctx);
        cairo_rectangle(ctx, xf.clipX * bounds.width, xf.clipY * bounds.height,
                        xf.clipW * bounds.width, xf.clipH * bounds.height);
        cairo_clip(ctx);
    }

    double cx = bounds.width / 2.0;
    double cy = bounds.height / 2.0;

    // Composite group: self content + children share this opacity layer.
    // Skipped for fully-opaque, childless elements: OVER compositing is
    // associative, so drawing self content straight onto the destination
    // matches grouping it and painting back at alpha 1.0 — at a fraction of the
    // cost (no full-size intermediate group surface per element per frame).
    // Note: not bit-identical at antialiased edges, since the group otherwise
    // adds an extra 8-bit quantization step on fractional-coverage pixels;
    // interiors are unchanged.
    const bool useGroup = !children.empty() || (double)opacity * xf.opacity < 1.0;
    if (useGroup)
        cairo_push_group(ctx);

    // Self content — full clip (corner radius + wipe + mask) inside save/restore
    // so it never bleeds into the children rendering below.
    cairo_save(ctx);

    if (shearX != 0.0f || shearY != 0.0f) {
        double cx = bounds.width / 2.0;
        double cy = bounds.height / 2.0;
        cairo_translate(ctx, cx, cy);
        cairo_matrix_t shearMat;
        cairo_matrix_init(&shearMat, 1.0, (double)shearY, (double)shearX, 1.0, 0.0, 0.0);
        cairo_transform(ctx, &shearMat);
        cairo_translate(ctx, -cx, -cy);
    }

    ApplyClipping(ctx, xf, maskXf, parentOffX, parentOffY);

    if (xf.scale != 1.0 && xf.scale > 1e-6) {
        cairo_translate(ctx, cx, cy);
        cairo_scale(ctx, xf.scale, xf.scale);
        cairo_translate(ctx, -cx, -cy);
    }

    if (rotation != 0.0f) {
        cairo_translate(ctx, cx, cy);
        cairo_rotate(ctx, rotation * Pi / 180.0);
        cairo_translate(ctx, -cx, -cy);
    }

    auto fnApplyFillAndStroke = [=, this]() {
        bool hasFill = fill.Valid();
        bool hasStroke = stroke.Valid() && strokeWidth > 0.0f;

        if (hasFill) {
            fill.Apply(ctx, bounds.width, bounds.height);
            if (hasStroke)
                cairo_fill_preserve(ctx);
            else
                cairo_fill(ctx);
        }

        if (hasStroke) {
            stroke.Apply(ctx, bounds.width, bounds.height);
            cairo_set_line_width(ctx, strokeWidth);
            cairo_stroke(ctx);
        }
    };

    switch (type) {
    case ElementType::Rectangle: {
        RoundRect(ctx, 0, 0, bounds.width, bounds.height, cornerRadius);
        fnApplyFillAndStroke();
        break;
    }

    case ElementType::Text: {
        std::string textXf = text;
        switch (textStyle.transform) {
        case TextTransform::Lowercase:
            std::transform(textXf.begin(), textXf.end(), textXf.begin(), ::tolower);
            break;
        case TextTransform::Uppercase:
            std::transform(textXf.begin(), textXf.end(), textXf.begin(), ::toupper);
            break;
        case TextTransform::Capitalize: {
            bool newWord = true;
            for (char& c : textXf) {
                c = ::tolower(c);
                if (::isspace(c)) {
                    newWord = true;
                } else if (newWord) {
                    c = ::toupper(c);
                    newWord = false;
                }
            }
        } break;
        default:
            break;
        }

        PangoAttrList* attrs = pango_attr_list_new();
        if (font.isUnderline) {
            pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
        }
        if (font.isStrikethrough) {
            pango_attr_list_insert(attrs, pango_attr_strikethrough_new(TRUE));
        }

        PangoLayout* layout = pango_cairo_create_layout(ctx);
        pango_layout_set_text(layout, textXf.c_str(), -1);
        pango_layout_set_attributes(layout, attrs);

        PangoFontDescription* fd = pango_font_description_new();
        if (!font.family.empty())
            pango_font_description_set_family(fd, font.family.c_str());
        pango_font_description_set_size(fd, static_cast<int>(font.size * PANGO_SCALE));
        pango_font_description_set_weight(fd, static_cast<PangoWeight>(font.weight));
        if (font.isItalic)
            pango_font_description_set_style(fd, PANGO_STYLE_ITALIC);

        pango_layout_set_font_description(layout, fd);

        static constexpr PangoAlignment kAlignMap[] = {PANGO_ALIGN_LEFT, PANGO_ALIGN_CENTER,
                                                       PANGO_ALIGN_LEFT, PANGO_ALIGN_RIGHT};
        pango_layout_set_alignment(layout, kAlignMap[(int)textStyle.alignX]);
        pango_layout_set_justify(layout, textStyle.alignX == HorizontalAlignment::Justify);

        double ox = 0.0, oy = 0.0;

        if (textStyle.autoScale) {
            pango_layout_set_width(layout, -1);
            PangoRectangle nat;
            pango_layout_get_pixel_extents(layout, &nat, nullptr);
            if (nat.width > 0 && nat.height > 0) {
                double sx = bounds.width / nat.width;
                double sy = bounds.height / nat.height;
                float newSize = font.size * (float)std::min(sx, sy);
                pango_font_description_set_size(fd, static_cast<int>(newSize * PANGO_SCALE));
                pango_layout_set_font_description(layout, fd);
            }
            PangoRectangle ink;
            pango_layout_get_pixel_extents(layout, &ink, nullptr);
            ox = -ink.x;
            oy = -ink.y;
        } else {
            pango_layout_set_width(layout, static_cast<int>(bounds.width * PANGO_SCALE));
            pango_layout_set_wrap(layout, static_cast<PangoWrapMode>(textStyle.wrapMode));
            if (textStyle.ellipsize != Ellipsize::None) {
                pango_layout_set_ellipsize(layout, static_cast<PangoEllipsizeMode>(textStyle.ellipsize));
                pango_layout_set_height(layout, static_cast<int>(bounds.height * PANGO_SCALE));
            }

            PangoRectangle ink;
            pango_layout_get_pixel_extents(layout, &ink, nullptr);
            switch (textStyle.alignY) {
            case VerticalAlignment::Top:
                oy = -ink.y;
                break;
            case VerticalAlignment::Bottom:
                oy = bounds.height - ink.height - ink.y;
                break;
            default:
                oy = (bounds.height - ink.height) / 2.0 - ink.y;
                break;
            }
        }

        pango_font_description_free(fd);

        if (fill.Valid()) {
            cairo_move_to(ctx, ox, oy);
            fill.Apply(ctx, bounds.width, bounds.height);
            pango_cairo_show_layout(ctx, layout);
        }

        if (stroke.Valid() && strokeWidth > 0.0f) {
            cairo_move_to(ctx, ox, oy);
            pango_cairo_layout_path(ctx, layout);
            stroke.Apply(ctx, bounds.width, bounds.height);
            cairo_set_line_width(ctx, strokeWidth);
            cairo_stroke(ctx);
        }

        pango_attr_list_unref(attrs);
        g_object_unref(layout);
        break;
    }

    case ElementType::Image: {
        if (!imagePath.empty() && m_imageCachePath != imagePath) {
            m_image = LoadImageSurface(imagePath);
            m_imageCachePath = imagePath;
        }
        auto* surface = m_image ? m_image.get() : nullptr;
        if (surface && cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
            int sw = cairo_image_surface_get_width(surface);
            int sh = cairo_image_surface_get_height(surface);
            if (sw > 0 && sh > 0) {
                double bw = bounds.width, bh = bounds.height;
                double ox = 0.0, oy = 0.0, sx = 1.0, sy = 1.0;

                switch (imageScaleMode) {
                case ScaleMode::Stretch:
                    sx = bw / sw;
                    sy = bh / sh;
                    break;
                case ScaleMode::Contain: {
                    double s = std::min(bw / sw, bh / sh);
                    sx = sy = s;
                    ox = (bw - sw * s) / 2.0;
                    oy = (bh - sh * s) / 2.0;
                    break;
                }
                case ScaleMode::Cover: {
                    double s = std::max(bw / sw, bh / sh);
                    sx = sy = s;
                    ox = (bw - sw * s) / 2.0;
                    oy = (bh - sh * s) / 2.0;
                    break;
                }
                case ScaleMode::FitWidth: {
                    double s = bw / sw;
                    sx = sy = s;
                    oy = (bh - sh * s) / 2.0;
                    break;
                }
                case ScaleMode::FitHeight: {
                    double s = bh / sh;
                    sx = sy = s;
                    ox = (bw - sw * s) / 2.0;
                    break;
                }
                case ScaleMode::None:
                    ox = (bw - sw) / 2.0;
                    oy = (bh - sh) / 2.0;
                    break;
                }

                cairo_save(ctx);
                // Always clip to element bounds — overflow is possible with Cover, None, FitW/H
                cairo_rectangle(ctx, 0, 0, bw, bh);
                cairo_clip(ctx);
                cairo_translate(ctx, ox, oy);
                if (imageScaleMode != ScaleMode::None)
                    cairo_scale(ctx, sx, sy);
                cairo_set_source_surface(ctx, surface, 0, 0);
                cairo_paint(ctx);
                cairo_restore(ctx);
            }
        }
        break;
    }

    case ElementType::QrCode: {
        if (m_oldText != text) {
            qr::encode_auto(m_qr, text.c_str(), text.size(), qr::Ecc::M);
            m_oldText = text;
        }

        if (m_qr.side_size() > 0) {
            double qrSizePx = std::min(bounds.width, bounds.height);
            double cellSize = qrSizePx / m_qr.side_size();
            double qrX = bounds.width / 2 - qrSizePx / 2;
            double qrY = bounds.height / 2 - qrSizePx / 2;

            cairo_save(ctx);
            cairo_translate(ctx, qrX, qrY);
            for (int y = 0; y < m_qr.side_size(); ++y) {
                for (int x = 0; x < m_qr.side_size(); ++x) {
                    if (m_qr.module(x, y)) {
                        cairo_rectangle(ctx, x * cellSize, y * cellSize, cellSize, cellSize);
                    }
                }
            }
            cairo_set_fill_rule(ctx, CAIRO_FILL_RULE_EVEN_ODD);
            if (fill.Valid() || (stroke.Valid() && strokeWidth > 0.0f)) {
                fnApplyFillAndStroke();
            } else {
                cairo_set_source_rgba(ctx, 0.0, 0.0, 0.0, 1.0);
                cairo_fill(ctx);
            }
            cairo_restore(ctx);
        }
        break;
    }
    }

    cairo_restore(ctx); // undo scale/rotation so children use the unmodified CTM

    // Children rendered on top, inside the group so parent opacity cascades to them
    if (!children.empty()) {
        std::vector<Element*> sorted(children.begin(), children.end());
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](Element* a, Element* b) { return a->zOrder < b->zOrder; });

        cairo_save(ctx);
        for (Element* child : sorted) {
            const auto& def = isOut ? child->outAnimation : child->inAnimation;
            AnimatedTransform childXf = animation::EvaluateAnimation(def, timer, isOut, *child);

            const AnimatedTransform* childMaskXf = nullptr;
            AnimatedTransform childMaskXfVal;
            if (child->mask) {
                const auto& mDef = isOut ? child->mask->outAnimation : child->mask->inAnimation;
                childMaskXfVal = animation::EvaluateAnimation(mDef, timer, isOut, *child->mask);
                childMaskXf = &childMaskXfVal;
            }

            cairo_save(ctx);
            cairo_translate(ctx, child->bounds.x, child->bounds.y);
            child->Render(ctx, childXf, childMaskXf, timer, isOut,
                          parentOffX + xf.offsetX, parentOffY + xf.offsetY);
            cairo_restore(ctx);
        }
        cairo_restore(ctx);
    }

    if (useGroup) {
        cairo_pop_group_to_source(ctx);
        cairo_paint_with_alpha(ctx, opacity * xf.opacity);
    }
}

void Element::ApplyClipping(cairo_t* ctx, const AnimatedTransform& xf,
                            const AnimatedTransform* maskXf,
                            double parentOffX, double parentOffY) const
{
    const bool isWiping = xf.clipX > 0.0 || xf.clipY > 0.0 || xf.clipW < 1.0 || xf.clipH < 1.0;
    const bool hasRadius =
        cornerRadius[0] > 0 || cornerRadius[1] > 0 || cornerRadius[2] > 0 || cornerRadius[3] > 0;

    if (isWiping || hasRadius) {
        RoundRect(ctx, 0, 0, bounds.width, bounds.height, cornerRadius);
        cairo_clip(ctx);
    }

    if (mask != nullptr) {
        Point maskGlobal = mask->GetGlobalPosition();
        Point selfGlobal = GetGlobalPosition();
        // CTM origin = selfGlobal + parentOff + xf.offset; subtract all to get clip-local coords
        float mx = (float)(maskGlobal.x - selfGlobal.x) - (float)xf.offsetX - (float)parentOffX;
        float my = (float)(maskGlobal.y - selfGlobal.y) - (float)xf.offsetY - (float)parentOffY;
        if (maskXf) {
            mx += (float)maskXf->offsetX;
            my += (float)maskXf->offsetY;
        }
        RoundRect(ctx, mx, my, mask->bounds.width, mask->bounds.height, mask->cornerRadius);
        cairo_clip(ctx);
    }

    if (isWiping) {
        cairo_rectangle(ctx, xf.clipX * bounds.width, xf.clipY * bounds.height,
                        xf.clipW * bounds.width, xf.clipH * bounds.height);
        cairo_clip(ctx);
    }
}

Point Element::GetGlobalPosition() const
{
    Point result{bounds.x, bounds.y};
    if (parent) {
        Point parentPos = parent->GetGlobalPosition();
        result.x += parentPos.x;
        result.y += parentPos.y;
    }
    return result;
}
