// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "render_util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "stb_image.h"

namespace render {

static inline uint32_t BlurRecip(uint32_t win)
{
    return (uint32_t)(((1ull << 24) + win - 1) / win); // ceil(2^24 / win)
}

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

void RoundRect(cairo_t* ctx, double x, double y, double w, double h, const float r[4])
{
    double tl = r[0], tr = r[1], br = r[2], bl = r[3];
    cairo_new_path(ctx);
    cairo_arc(ctx, x + tl, y + tl, tl, Pi, 3 * Pi / 2);
    cairo_arc(ctx, x + w - tr, y + tr, tr, 3 * Pi / 2, 0);
    cairo_arc(ctx, x + w - br, y + h - br, br, 0, Pi / 2);
    cairo_arc(ctx, x + bl, y + h - bl, bl, Pi / 2, Pi);
    cairo_close_path(ctx);
}

std::shared_ptr<cairo_surface_t> LoadImageSurface(const std::string& path)
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

static void BlurAndPaintA8(cairo_t* ctx, cairo_surface_t* a8, int W, int H, int pad,
                            const int radii[3], double originX, double originY,
                            double r, double g, double b, double a)
{
    cairo_surface_flush(a8);
    uint8_t* data = cairo_image_surface_get_data(a8);
    const int stride = cairo_image_surface_get_stride(a8);
    std::vector<uint8_t> tmp((size_t)W * H);
    std::vector<uint32_t> colsum;
    for (int i = 0; i < 3; ++i) {
        BoxBlurH(data, stride, tmp.data(), W, W, H, radii[i]);
        BoxBlurV(tmp.data(), W, data, stride, W, H, radii[i], colsum);
    }
    cairo_surface_mark_dirty(a8);
    cairo_set_source_rgba(ctx, r, g, b, a);
    cairo_mask_surface(ctx, a8, originX - pad, originY - pad);
}

void RenderDropShadow(cairo_t* ctx, double sx, double sy, double sw, double sh,
                      double blur, double r, double g, double b, double a, float cornerR)
{
    if (a < 1e-6 || sw <= 0.0 || sh <= 0.0 || blur <= 0.0)
        return;

    int radii[3];
    GaussBoxRadii(blur, radii);
    const int pad = radii[0] + radii[1] + radii[2] + 1;

    const int W = (int)std::ceil(sw) + 2 * pad;
    const int H = (int)std::ceil(sh) + 2 * pad;
    if (W <= 0 || H <= 0)
        return;

    cairo_surface_t* a8 = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
    {
        cairo_t* cc = cairo_create(a8);
        cairo_translate(cc, pad - sx, pad - sy);
        float cr = std::clamp(cornerR, 0.0f, (float)(std::min(sw, sh) / 2.0));
        float rr[4] = {cr, cr, cr, cr};
        RoundRect(cc, sx, sy, sw, sh, rr);
        cairo_set_source_rgba(cc, 0, 0, 0, 1.0);
        cairo_fill(cc);
        cairo_destroy(cc);
    }

    BlurAndPaintA8(ctx, a8, W, H, pad, radii, sx, sy, r, g, b, a);
    cairo_surface_destroy(a8);
}

void RenderDropShadowFromSurface(cairo_t* ctx, cairo_surface_t* srcA8,
                                  double destX, double destY,
                                  double blur, double r, double g, double b, double a)
{
    if (a < 1e-6 || blur <= 0.0)
        return;

    const int srcW = cairo_image_surface_get_width(srcA8);
    const int srcH = cairo_image_surface_get_height(srcA8);
    if (srcW <= 0 || srcH <= 0)
        return;

    int radii[3];
    GaussBoxRadii(blur, radii);
    const int pad = radii[0] + radii[1] + radii[2] + 1;

    const int W = srcW + 2 * pad;
    const int H = srcH + 2 * pad;

    cairo_surface_t* a8 = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);

    cairo_surface_flush(srcA8);
    const uint8_t* srcData = cairo_image_surface_get_data(srcA8);
    const int srcStride    = cairo_image_surface_get_stride(srcA8);
    uint8_t* dstData       = cairo_image_surface_get_data(a8);
    const int dstStride    = cairo_image_surface_get_stride(a8);
    for (int y = 0; y < srcH; ++y)
        std::memcpy(dstData + (size_t)(y + pad) * dstStride + pad,
                    srcData + (size_t)y * srcStride,
                    (size_t)srcW);
    cairo_surface_mark_dirty(a8);

    BlurAndPaintA8(ctx, a8, W, H, pad, radii, destX, destY, r, g, b, a);
    cairo_surface_destroy(a8);
}

} // namespace render
