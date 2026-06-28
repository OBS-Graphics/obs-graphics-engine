// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "element_rectangle.h"
#include "render_util.h"

void RectangleElement::RenderContent(cairo_t* ctx) const
{
    render::RoundRect(ctx, 0, 0, m_bounds.width, m_bounds.height, cornerRadius);
    ApplyFillAndStroke(ctx);
}
