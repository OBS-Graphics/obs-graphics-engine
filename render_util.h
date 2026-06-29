// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "types.hpp"
#include <memory>
#include <string>

namespace render {

void RoundRect(cairo_t* ctx, double x, double y, double w, double h, const float r[4]);

std::shared_ptr<cairo_surface_t> LoadImageSurface(const std::string& path);

void RenderDropShadow(cairo_t* ctx, double sx, double sy, double sw, double sh,
                      double blur, double r, double g, double b, double a, float cornerR);

// Blur and composite a pre-rendered A8 surface as a drop shadow.
// srcA8 must be CAIRO_FORMAT_A8. (destX, destY) is where the surface's (0,0) lands.
void RenderDropShadowFromSurface(cairo_t* ctx, cairo_surface_t* srcA8,
                                  double destX, double destY,
                                  double blur, double r, double g, double b, double a);

} // namespace render
