// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "element_text.h"

#include <algorithm>

PangoLayout* TextElement::BuildLayout(cairo_t* ctx, double& ox, double& oy) const
{
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
            if (::isspace(c))      newWord = true;
            else if (newWord)    { c = ::toupper(c); newWord = false; }
        }
    } break;
    default:
        break;
    }

    PangoAttrList* attrs = pango_attr_list_new();
    if (font.isUnderline)
        pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
    if (font.isStrikethrough)
        pango_attr_list_insert(attrs, pango_attr_strikethrough_new(TRUE));

    PangoLayout* layout = pango_cairo_create_layout(ctx);
    pango_layout_set_text(layout, textXf.c_str(), -1);
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);

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

    ox = 0.0;
    oy = 0.0;

    if (textStyle.autoScale) {
        pango_layout_set_width(layout, -1);
        PangoRectangle nat;
        pango_layout_get_pixel_extents(layout, &nat, nullptr);
        if (nat.width > 0 && nat.height > 0) {
            double sx = m_bounds.width / nat.width;
            double sy = m_bounds.height / nat.height;
            float newSize = font.size * (float)std::min(sx, sy);
            pango_font_description_set_size(fd, static_cast<int>(newSize * PANGO_SCALE));
            pango_layout_set_font_description(layout, fd);
        }
        PangoRectangle ink;
        pango_layout_get_pixel_extents(layout, &ink, nullptr);
        ox = -ink.x;
        oy = -ink.y;
    } else {
        pango_layout_set_width(layout, static_cast<int>(m_bounds.width * PANGO_SCALE));
        pango_layout_set_wrap(layout, static_cast<PangoWrapMode>(textStyle.wrapMode));
        if (textStyle.ellipsize != Ellipsize::None) {
            pango_layout_set_ellipsize(layout, static_cast<PangoEllipsizeMode>(textStyle.ellipsize));
            pango_layout_set_height(layout, static_cast<int>(m_bounds.height * PANGO_SCALE));
        }
        PangoRectangle ink;
        pango_layout_get_pixel_extents(layout, &ink, nullptr);
        switch (textStyle.alignY) {
        case VerticalAlignment::Top:    oy = -ink.y;                                       break;
        case VerticalAlignment::Bottom: oy = m_bounds.height - ink.height - ink.y;         break;
        default:                        oy = (m_bounds.height - ink.height) / 2.0 - ink.y; break;
        }
    }

    pango_font_description_free(fd);
    return layout;
}

void TextElement::RenderContent(cairo_t* ctx) const
{
    double ox, oy;
    PangoLayout* layout = BuildLayout(ctx, ox, oy);

    if (fill.Valid()) {
        cairo_move_to(ctx, ox, oy);
        fill.Apply(ctx, m_bounds.width, m_bounds.height);
        pango_cairo_show_layout(ctx, layout);
    }

    if (stroke.Valid() && strokeWidth > 0.0f) {
        cairo_move_to(ctx, ox, oy);
        pango_cairo_layout_path(ctx, layout);
        stroke.Apply(ctx, m_bounds.width, m_bounds.height);
        cairo_set_line_width(ctx, strokeWidth);
        cairo_stroke(ctx);
    }

    g_object_unref(layout);
}

cairo_surface_t* TextElement::CreateShadowSurface(int w, int h) const
{
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
    cairo_t* cc = cairo_create(surf);
    double ox, oy;
    PangoLayout* layout = BuildLayout(cc, ox, oy);
    cairo_set_source_rgba(cc, 0, 0, 0, 1.0);
    cairo_move_to(cc, ox, oy);
    pango_cairo_show_layout(cc, layout);
    g_object_unref(layout);
    cairo_destroy(cc);
    return surf;
}
