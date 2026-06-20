#pragma once

#include <memory>
#include <pango/pangocairo.h>
#include <string>
#include <vector>

#include "animation.h"
#include "types.hpp"
#include "qr.hpp"

enum class ElementType { Rectangle, Text, Image, QrCode };

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

enum class ScaleMode { Stretch, Contain, Cover, FitWidth, FitHeight, None };

struct Element {
    std::string id;
    ElementType type;

    int zOrder{0};

    AnimationDef inAnimation{}, outAnimation{};

    Rectangle bounds{0.0, 0.0, 100.0, 100.0};
    float rotation{0.0f};

    Paint fill, stroke;
    float strokeWidth{0.0f};
    float cornerRadius[4]{0.0f, 0.0f, 0.0f, 0.0f};

    float opacity{1.0f};

    Element* mask{nullptr};
    Element* parent{nullptr};
    std::vector<Element*> children;

    bool fitToChildren{false};
    float childrenPadding[4]{0.0f, 0.0f, 0.0f, 0.0f}; // top, right, bottom, left

    struct {
        std::string family;
        float size{36.0f};
        FontWeight weight{FontWeight::Normal};
        bool isItalic{false};
        bool isUnderline{false};
        bool isStrikethrough{false};
    } font{};

    std::string text;

    struct {
        bool autoScale{false};
        HorizontalAlignment alignX{HorizontalAlignment::Left};
        VerticalAlignment alignY{VerticalAlignment::Top};
        Ellipsize ellipsize{Ellipsize::None};
        WrapMode wrapMode{WrapMode::Word};
        TextTransform transform{TextTransform::None};
    } textStyle{};

    std::string imagePath{};
    ScaleMode imageScaleMode{ScaleMode::Stretch};

    void Render(cairo_t* ctx, const AnimatedTransform& xf,
                const AnimatedTransform* maskXf,
                double timer, bool isOut,
                double parentOffX = 0.0, double parentOffY = 0.0) const;
    void ApplyClipping(cairo_t* ctx, const AnimatedTransform& xf,
                       const AnimatedTransform* maskXf,
                       double parentOffX, double parentOffY) const;
    Point GetGlobalPosition() const;

private:
    mutable std::string m_oldText{"\xff"};
    mutable qr::DynQr m_qr{};
    mutable std::shared_ptr<cairo_surface_t> m_image{};
    mutable std::string m_imageCachePath{};
};
