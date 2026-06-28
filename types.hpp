// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#ifdef __APPLE__
#include <cairo.h>
#else
#include <cairo/cairo.h>
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

constexpr double Pi = std::numbers::pi;

enum class ScaleMode { Stretch, Contain, Cover, FitWidth, FitHeight, None, Tile };

struct Point {
    double x, y;
};

struct Size {
    double width, height;
};

struct Rectangle {
    double x, y, width, height;
};

struct Paint {
    enum class Type { None, Solid, Linear, Radial, Image };
    struct Stop {
        double offset, r, g, b, a;
    };

    Type type{Type::None};
    // Solid: r,g,b,a  |  Linear: x0,y0,x1,y1  |  Radial: cx0,cy0,r0,cx1,cy1,r1
    double params[6]{};
    std::string imagePath{};
    std::vector<Stop> stops;
    ScaleMode imageScaleMode{ScaleMode::Stretch};
    double tileScale{1.0};

    Paint() = default;

    inline static Paint Solid(double r, double g, double b, double a = 1.0)
    {
        Paint p;
        p.type = Type::Solid;
        p.params[0] = r;
        p.params[1] = g;
        p.params[2] = b;
        p.params[3] = a;
        return p;
    }

    // Normalized coords (0–1); multiplied by element bounds in Apply
    inline static Paint Linear(double x0, double y0, double x1, double y1)
    {
        Paint p;
        p.type = Type::Linear;
        p.params[0] = x0;
        p.params[1] = y0;
        p.params[2] = x1;
        p.params[3] = y1;
        return p;
    }

    // cx/cy normalized by width/height; r normalized by width
    inline static Paint Radial(double cx0, double cy0, double r0, double cx1, double cy1, double r1)
    {
        Paint p;
        p.type = Type::Radial;
        p.params[0] = cx0;
        p.params[1] = cy0;
        p.params[2] = r0;
        p.params[3] = cx1;
        p.params[4] = cy1;
        p.params[5] = r1;
        return p;
    }

    inline static Paint Image(const std::string& path)
    {
        Paint p{};
        p.type = Type::Image;
        p.imagePath = path;
        p.m_patternImage = std::shared_ptr<cairo_surface_t>(
            cairo_image_surface_create_from_png(path.c_str()),
            cairo_surface_destroy);
        return p;
    }

    inline void AddStop(double offset, double r, double g, double b, double a = 1.0)
    {
        stops.push_back({offset, r, g, b, a});
    }

    inline bool Valid() const
    {
        return type != Type::None;
    }

    inline void Apply(cairo_t* cr, double w, double h) const
    {
        if (type == Type::None)
            return;

        if (type == Type::Solid) {
            cairo_set_source_rgba(cr, params[0], params[1], params[2], params[3]);
            return;
        }

        if (type == Type::Image) {
            if (!m_patternImage) return;
            auto* surface = m_patternImage.get();
            if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) return;
            int sw = cairo_image_surface_get_width(surface);
            int sh = cairo_image_surface_get_height(surface);
            if (sw <= 0 || sh <= 0) return;

            cairo_pattern_t* pat = cairo_pattern_create_for_surface(surface);
            cairo_matrix_t m;

            if (imageScaleMode == ScaleMode::Tile) {
                double ts = (tileScale > 1e-9) ? tileScale : 1.0;
                cairo_matrix_init_scale(&m, 1.0 / ts, 1.0 / ts);
                cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
            } else {
                double ox = 0.0, oy = 0.0, s = 1.0;
                switch (imageScaleMode) {
                case ScaleMode::Contain:
                    s = std::min(w / sw, h / sh);
                    ox = (w - sw * s) / 2.0;
                    oy = (h - sh * s) / 2.0;
                    break;
                case ScaleMode::Cover:
                    s = std::max(w / sw, h / sh);
                    ox = (w - sw * s) / 2.0;
                    oy = (h - sh * s) / 2.0;
                    break;
                case ScaleMode::FitWidth:
                    s = w / sw;
                    oy = (h - sh * s) / 2.0;
                    break;
                case ScaleMode::FitHeight:
                    s = h / sh;
                    ox = (w - sw * s) / 2.0;
                    break;
                case ScaleMode::None:
                    s = 1.0;
                    ox = (w - sw) / 2.0;
                    oy = (h - sh) / 2.0;
                    break;
                default: // Stretch
                    cairo_matrix_init(&m, (double)sw / w, 0, 0, (double)sh / h, 0, 0);
                    cairo_pattern_set_matrix(pat, &m);
                    cairo_set_source(cr, pat);
                    cairo_pattern_destroy(pat);
                    return;
                }
                // All non-Stretch, non-Tile modes: uniform scale s with centering offset
                cairo_matrix_init(&m, 1.0 / s, 0, 0, 1.0 / s, -ox / s, -oy / s);
            }

            cairo_pattern_set_matrix(pat, &m);
            cairo_set_source(cr, pat);
            cairo_pattern_destroy(pat);
            return;
        }

        cairo_pattern_t* pat;
        if (type == Type::Linear) {
            pat = cairo_pattern_create_linear(params[0] * w, params[1] * h, params[2] * w,
                                              params[3] * h);
        } else {
            pat = cairo_pattern_create_radial(params[0] * w, params[1] * h, params[2] * w,
                                              params[3] * w, params[4] * h, params[5] * w);
        }
        for (const auto& s : stops)
            cairo_pattern_add_color_stop_rgba(pat, s.offset, s.r, s.g, s.b, s.a);
        cairo_set_source(cr, pat);
        cairo_pattern_destroy(pat);
    }

protected:
    std::shared_ptr<cairo_surface_t> m_patternImage{};
};
