#include "element.h"
#include "animation.h"

#include <algorithm>
#include <memory>

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

    // Wipe clip only — applied before the group so children are revealed/hidden
    // with the parent during wipe animations. Corner radius is intentionally
    // excluded here: it is cosmetic for the parent shape and must not clip children.
    const bool isWiping = xf.clipX > 0.0 || xf.clipY > 0.0 || xf.clipW < 1.0 || xf.clipH < 1.0;
    if (isWiping) {
        cairo_rectangle(ctx, 0, 0, bounds.width, bounds.height);
        cairo_clip(ctx);
        cairo_rectangle(ctx, xf.clipX * bounds.width, xf.clipY * bounds.height,
                        xf.clipW * bounds.width, xf.clipH * bounds.height);
        cairo_clip(ctx);
    }

    double cx = bounds.width / 2.0;
    double cy = bounds.height / 2.0;

    // Composite group: self content + children share this opacity layer
    cairo_push_group(ctx);

    // Self content — full clip (corner radius + wipe + mask) inside save/restore
    // so it never bleeds into the children rendering below.
    cairo_save(ctx);

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
            m_image = std::shared_ptr<cairo_surface_t>(
                cairo_image_surface_create_from_png(imagePath.c_str()),
                cairo_surface_destroy);
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
            fnApplyFillAndStroke();
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

    cairo_pop_group_to_source(ctx);
    cairo_paint_with_alpha(ctx, opacity * xf.opacity);
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
