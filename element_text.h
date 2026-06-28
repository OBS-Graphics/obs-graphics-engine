// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "visual_element.h"
#include <pango/pangocairo.h>
#include <string>

enum class FontWeight {
    Normal = PANGO_WEIGHT_NORMAL,
    Medium = PANGO_WEIGHT_MEDIUM,
    Bold = PANGO_WEIGHT_BOLD,
    SemiBold = PANGO_WEIGHT_SEMIBOLD,
    UltraBold = PANGO_WEIGHT_ULTRABOLD,
    Light = PANGO_WEIGHT_LIGHT,
    SemiLight = PANGO_WEIGHT_SEMILIGHT,
    UltraLight = PANGO_WEIGHT_ULTRALIGHT,
    Thin = PANGO_WEIGHT_THIN,
    UltraThin = PANGO_WEIGHT_THIN,
    Heavy = PANGO_WEIGHT_HEAVY,
    UltraHeavy = PANGO_WEIGHT_ULTRAHEAVY,
    Book = PANGO_WEIGHT_BOOK
};

enum class HorizontalAlignment { Left = 0, Center, Justify, Right };

enum class VerticalAlignment { Top = 0, Middle, Bottom };

enum class Ellipsize {
    None = PANGO_ELLIPSIZE_NONE,
    Start = PANGO_ELLIPSIZE_START,
    Middle = PANGO_ELLIPSIZE_MIDDLE,
    End = PANGO_ELLIPSIZE_END
};

enum class WrapMode {
    Word = PANGO_WRAP_WORD,
    Char = PANGO_WRAP_CHAR,
    WordChar = PANGO_WRAP_WORD_CHAR
};

enum class TextTransform { None = 0, Capitalize, Uppercase, Lowercase };

class TextElement : public VisualElement {
public:
    struct Font {
        std::string family;
        float size{36.0f};
        FontWeight weight{FontWeight::Normal};
        bool isItalic{false};
        bool isUnderline{false};
        bool isStrikethrough{false};
    } font{};

    std::string text;

    struct TextStyle {
        bool autoScale{false};
        HorizontalAlignment alignX{HorizontalAlignment::Left};
        VerticalAlignment alignY{VerticalAlignment::Top};
        Ellipsize ellipsize{Ellipsize::None};
        WrapMode wrapMode{WrapMode::Word};
        TextTransform transform{TextTransform::None};
    } textStyle{};

    void SetContent(const std::string& value) override { text = value; }

protected:
    void RenderContent(cairo_t* ctx) const override;
};
