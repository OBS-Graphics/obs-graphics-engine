// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "element_qr.h"

void QrElement::RenderContent(cairo_t* ctx) const
{
    if (m_oldText != text) {
        qr::encode_auto(m_qr, text.c_str(), text.size(), qr::Ecc::M);
        m_oldText = text;
    }

    if (m_qr.side_size() <= 0)
        return;

    double qrSizePx = std::min(m_bounds.width, m_bounds.height);
    double cellSize = qrSizePx / m_qr.side_size();
    double qrX = m_bounds.width / 2 - qrSizePx / 2;
    double qrY = m_bounds.height / 2 - qrSizePx / 2;

    cairo_save(ctx);
    cairo_translate(ctx, qrX, qrY);
    for (int y = 0; y < m_qr.side_size(); ++y) {
        for (int x = 0; x < m_qr.side_size(); ++x) {
            if (m_qr.module(x, y))
                cairo_rectangle(ctx, x * cellSize, y * cellSize, cellSize, cellSize);
        }
    }
    cairo_set_fill_rule(ctx, CAIRO_FILL_RULE_EVEN_ODD);
    if (fill.Valid() || (stroke.Valid() && strokeWidth > 0.0f)) {
        ApplyFillAndStroke(ctx);
    } else {
        cairo_set_source_rgba(ctx, 0.0, 0.0, 0.0, 1.0);
        cairo_fill(ctx);
    }
    cairo_restore(ctx);
}
