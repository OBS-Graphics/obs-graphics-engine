// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "element_image.h"
#include "render_util.h"

#include <algorithm>
#include <cmath>

void ImageElement::RenderContent(cairo_t* ctx) const
{
    if (!imagePath.empty() && m_imageCachePath != imagePath) {
        m_image = render::LoadImageSurface(imagePath);
        m_imageCachePath = imagePath;
    }

    auto* surface = m_image ? m_image.get() : nullptr;
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
        return;

    int sw = cairo_image_surface_get_width(surface);
    int sh = cairo_image_surface_get_height(surface);
    if (sw <= 0 || sh <= 0)
        return;

    double bw = m_bounds.width, bh = m_bounds.height;

    cairo_save(ctx);
    cairo_rectangle(ctx, 0, 0, bw, bh);
    cairo_clip(ctx);

    if (imageScaleMode == ScaleMode::Tile) {
        double ts = (imageTileScale > 1e-9) ? imageTileScale : 1.0;
        cairo_pattern_t* pat = cairo_pattern_create_for_surface(surface);
        cairo_matrix_t mat;
        cairo_matrix_init_scale(&mat, 1.0 / ts, 1.0 / ts);
        cairo_pattern_set_matrix(pat, &mat);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
        cairo_set_source(ctx, pat);
        cairo_pattern_destroy(pat);
        cairo_paint(ctx);
    } else {
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
        default:
            break;
        }
        cairo_translate(ctx, ox, oy);
        if (imageScaleMode != ScaleMode::None)
            cairo_scale(ctx, sx, sy);
        cairo_set_source_surface(ctx, surface, 0, 0);
        cairo_paint(ctx);
    }

    cairo_restore(ctx);
}
