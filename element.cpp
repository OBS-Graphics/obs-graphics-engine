#include "element.h"
#include "animation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

static void RoundRect(cairo_t* ctx, double x, double y, double w, double h, const float r[4]);

// Fixed-point reciprocal of the window size, so the per-pixel normalization is a
// multiply+shift instead of an integer division (the hot-loop bottleneck).
static inline uint32_t BlurRecip(uint32_t win)
{
    return (uint32_t)(((1ull << 24) + win - 1) / win); // ceil(2^24 / win)
}

// Horizontal box pass (radius r), clamp-to-edge, O(w) running sum. Separate src
// and dst buffers (row-sequential reads and writes — cache friendly).
static void BoxBlurH(const uint8_t* src, int sstride, uint8_t* dst, int dstride, int w, int h,
                     int radius)
{
    const uint32_t win = 2u * (uint32_t)radius + 1u;
    const uint32_t recip = BlurRecip(win);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + (size_t)y * sstride;
        uint8_t* d = dst + (size_t)y * dstride;
        uint32_t sum = 0;
        for (int k = -radius; k <= radius; ++k)
            sum += s[std::clamp(k, 0, w - 1)];
        d[0] = (uint8_t)(((uint64_t)sum * recip) >> 24);
        for (int x = 1; x < w; ++x) {
            sum += s[std::clamp(x + radius, 0, w - 1)];
            sum -= s[std::clamp(x - radius - 1, 0, w - 1)];
            d[x] = (uint8_t)(((uint64_t)sum * recip) >> 24);
        }
    }
}

// Vertical box pass (radius r), clamp-to-edge. Maintains a per-column running sum
// and walks output rows top-to-bottom, so all reads/writes are row-sequential
// (no column striding). `colsum` is reused scratch (one uint32 per column).
static void BoxBlurV(const uint8_t* src, int sstride, uint8_t* dst, int dstride, int w, int h,
                     int radius, std::vector<uint32_t>& colsum)
{
    const uint32_t win = 2u * (uint32_t)radius + 1u;
    const uint32_t recip = BlurRecip(win);
    colsum.assign(w, 0);
    for (int k = -radius; k <= radius; ++k) {
        const uint8_t* s = src + (size_t)std::clamp(k, 0, h - 1) * sstride;
        for (int x = 0; x < w; ++x)
            colsum[x] += s[x];
    }
    for (int x = 0; x < w; ++x)
        dst[x] = (uint8_t)(((uint64_t)colsum[x] * recip) >> 24);
    for (int y = 1; y < h; ++y) {
        const uint8_t* add = src + (size_t)std::clamp(y + radius, 0, h - 1) * sstride;
        const uint8_t* sub = src + (size_t)std::clamp(y - radius - 1, 0, h - 1) * sstride;
        uint8_t* d = dst + (size_t)y * dstride;
        for (int x = 0; x < w; ++x) {
            colsum[x] += add[x];
            colsum[x] -= sub[x];
            d[x] = (uint8_t)(((uint64_t)colsum[x] * recip) >> 24);
        }
    }
}

// Three integer box radii whose 3x application approximates a Gaussian of the
// given sigma (Jarosz "fast almost-Gaussian filtering"); all boxes are odd-sized
// so each is a symmetric radius.
static void GaussBoxRadii(double sigma, int radii[3])
{
    const int n = 3;
    double wIdeal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
    int wl = (int)std::floor(wIdeal);
    if (wl % 2 == 0)
        wl -= 1;
    int wu = wl + 2;
    double mIdeal =
        (12.0 * sigma * sigma - n * (double)wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0);
    int m = (int)std::lround(mIdeal);
    for (int i = 0; i < n; ++i)
        radii[i] = (((i < m) ? wl : wu) - 1) / 2;
}

// Drop shadow: a 3x box-blur (~Gaussian, per the SVG feGaussianBlur recipe) of
// the element shape, rasterized into a padded A8 surface and composited as a
// coloured shadow via cairo_mask_surface(). 'a' is pre-baked with element opacity.
// Cost is O(pixels) regardless of blur radius. Rendered per-frame for now.
void Element::RenderDropShadow(cairo_t* ctx, double sx, double sy, double sw, double sh,
                               double blur, double r, double g, double b, double a,
                               float cornerR_in) const
{
    if (a < 1e-6 || sw <= 0.0 || sh <= 0.0 || blur <= 0.0)
        return;

    const double sigma = blur; // map shadow.blur -> Gaussian sigma
    int radii[3];
    GaussBoxRadii(sigma, radii);
    const int pad = radii[0] + radii[1] + radii[2] + 1; // total blur spread + 1px safety

    const int W = (int)std::ceil(sw) + 2 * pad;
    const int H = (int)std::ceil(sh) + 2 * pad;
    if (W <= 0 || H <= 0)
        return;

    // Rasterize the shape into a padded A8 buffer, shape top-left at (pad, pad).
    cairo_surface_t* a8 = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
    {
        cairo_t* cc = cairo_create(a8);
        cairo_translate(cc, pad - sx, pad - sy);
        float cr = std::clamp(cornerR_in, 0.0f, (float)(std::min(sw, sh) / 2.0));
        float rr[4] = {cr, cr, cr, cr};
        RoundRect(cc, sx, sy, sw, sh, rr);
        cairo_set_source_rgba(cc, 0, 0, 0, 1.0); // A8: only the alpha is stored
        cairo_fill(cc);
        cairo_destroy(cc);
    }

    cairo_surface_flush(a8);
    uint8_t* data = cairo_image_surface_get_data(a8);
    const int stride = cairo_image_surface_get_stride(a8);
    // Ping-pong between the A8 surface and a tightly-packed scratch plane: each
    // box blur is H (surface -> tmp) then V (tmp -> surface), so the result lands
    // back in the surface after every box.
    std::vector<uint8_t> tmp((size_t)W * H);
    std::vector<uint32_t> colsum;
    for (int i = 0; i < 3; ++i) {
        BoxBlurH(data, stride, tmp.data(), W, W, H, radii[i]);
        BoxBlurV(tmp.data(), W, data, stride, W, H, radii[i], colsum);
    }
    cairo_surface_mark_dirty(a8);

    // Paint (r,g,b,a) through the blurred A8 mask, placing A8(0,0) at the shape's
    // padded local origin so the shadow lands at (sx, sy) incl. its offset.
    cairo_set_source_rgba(ctx, r, g, b, a);
    cairo_mask_surface(ctx, a8, sx - pad, sy - pad);

    cairo_surface_destroy(a8);
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
        RenderDropShadow(ctx, sx, sy, sw, sh, (double)shadow.blur,
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
