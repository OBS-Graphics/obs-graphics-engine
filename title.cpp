// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "title.h"

#include "data-pool.h"
#include "element_image.h"
#include "element_qr.h"
#include "element_rectangle.h"
#include "element_text.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <zipper.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

static constexpr int kOgtSchemaMajor = 1;
static constexpr int kOgtSchemaMinor = 1;

// Read-compatibility classification for a file's (major, minor) schema
// version against the schema this build writes/understands.
enum class SchemaCompat {
    Ok,           // file schema <= what we support: load normally.
    OlderMigrate, // file major < ours: content migrations may be needed.
    NewerMinor,   // same major, file minor is ahead: unknown fields are
                  // preserved (see kKnownTitleKeys/title.extra), so this is
                  // safe to load as-is.
    NewerMajor,   // file major is ahead: fields may have been repurposed or
                  // removed, not just added. Loaded best-effort/degraded.
};

// Not `static`: kept internal (no title.h declaration, not part of the
// public API) but with external linkage so a standalone test binary can
// forward-declare and link against it directly.
SchemaCompat ClassifySchema(int fileMajor, int fileMinor)
{
    if (fileMajor < kOgtSchemaMajor) return SchemaCompat::OlderMigrate;
    if (fileMajor == kOgtSchemaMajor) {
        return (fileMinor <= kOgtSchemaMinor) ? SchemaCompat::Ok
                                               : SchemaCompat::NewerMinor;
    }
    return SchemaCompat::NewerMajor;
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

Title::Title()
{
    auto root = std::make_unique<IElement>();
    root->SetId("__root");
    elements.push_back(std::move(root));
}

Title::~Title()
{
    if (!m_tempAssetDir.empty()) {
        std::error_code ec;
        fs::remove_all(m_tempAssetDir, ec);
    }
}

// ── Runtime API ───────────────────────────────────────────────────────────────

VisualElement& Title::GetById(const std::string& id)
{
    for (size_t i = 1; i < elements.size(); ++i) {
        if (elements[i]->GetId() == id)
            return *static_cast<VisualElement*>(elements[i].get());
    }
    throw std::runtime_error("Element with id '" + id + "' not found");
}

void Title::TriggerIn(size_t recordIndex, double duration)
{
    dataRecordIndex = recordIndex;

    // A title coming from Hidden must show real, current data the moment it
    // appears — hence the blocking fetch rather than whatever the pool
    // happens to have cached. DataBlocking is bounded and never propagates a
    // source's exceptions (see data-pool.h).
    if (dataPool && !dataSourceId.empty()) {
        UpdateData(dataPool->DataBlocking(dataSourceId), /*instant=*/true);
        m_lastDataVersion = dataPool->DataVersion(dataSourceId);
    }

    EnterAnimatingIn(recordIndex, duration);
}

void Title::TriggerIn(size_t recordIndex, double duration, const std::vector<Record>& records)
{
    dataRecordIndex = recordIndex;

    UpdateData(records, /*instant=*/true);

    // Sync to whatever the cache is at now. The caller's own fetch already
    // refreshed (and version-bumped) it, so without this the first Visible
    // tick would read that bump as a change and re-apply the very same content
    // through the *animated* SetContent — a visible re-animation of what the
    // title is already showing. Non-blocking: DataVersion is a map lookup.
    if (dataPool && !dataSourceId.empty())
        m_lastDataVersion = dataPool->DataVersion(dataSourceId);

    EnterAnimatingIn(recordIndex, duration);
}

void Title::EnterAnimatingIn(size_t recordIndex, double duration)
{
    state = TitleState::AnimatingIn;
    timer = 0.0;
    this->duration = duration;

    for (auto& cb : onTriggerIn)
        if (cb) cb(recordIndex, duration);

    if (dataPool && !dataSourceId.empty())
        dataPool->NotifyTriggerIn(dataSourceId, Ref(), recordIndex, duration);
}

void Title::TriggerOut()
{
    state = TitleState::AnimatingOut;
    timer = 0.0;

    for (auto& cb : onTriggerOut)
        if (cb) cb();

    if (dataPool && !dataSourceId.empty())
        dataPool->NotifyTriggerOut(dataSourceId, Ref());
}

void Title::UpdateData()
{
    if (!dataPool || dataSourceId.empty()) return;

    std::vector<Record> records;
    if (!dataPool->DataIfChanged(dataSourceId, m_lastDataVersion, records))
        return;  // cache unchanged since the last pull — nothing to animate

    UpdateData(records, /*instant=*/false);
}

void Title::UpdateData(const std::vector<Record>& records, bool instant)
{
    if (records.empty()) return;

    const auto& record = records[dataRecordIndex % records.size()];
    for (auto&& [key, value] : record) {
        try {
            auto& el = GetById(key);
            if (instant)
                el.SetContentInstant(value);
            else
                el.SetContent(value);
        } catch (const std::runtime_error&) {
            // no element with this id — ignore
        }
    }
}

void Title::Tick(float dt)
{
    if (state == TitleState::Visible) {
        // Polled every frame rather than on a timer of its own: the pool owns
        // the fetch cadence, and an unchanged cache costs one integer compare
        // here (see DataPool::DataIfChanged).
        UpdateData();

        for (size_t i = 1; i < elements.size(); ++i)
            static_cast<VisualElement*>(elements[i].get())->TickData(dt);

        if (duration >= 0.0) {
            timer += dt;
            if (timer >= duration)
                TriggerOut();
        }

        return;
    }

    if (state == TitleState::Hidden)
        return;

    timer += dt;

    bool allDone = true;
    for (size_t i = 1; i < elements.size(); ++i) {
        const auto* ve = static_cast<const VisualElement*>(elements[i].get());
        const auto& def = (state == TitleState::AnimatingIn) ? ve->inAnimation : ve->outAnimation;
        if (timer < def.delay + def.duration) {
            allDone = false;
            break;
        }
    }

    if (allDone) {
        state = (state == TitleState::AnimatingIn) ? TitleState::Visible : TitleState::Hidden;
        if (state == TitleState::Visible)
            timer = 0.0;
    }
}

void Title::RenderElements(cairo_t* ctx) const
{
    auto* root = GetRoot();
    if (!root) return;

    bool isOut = (state == TitleState::AnimatingOut);
    bool isVisible = (state == TitleState::Visible);

    std::vector<VisualElement*> rootChildren;
    for (IElement* child : root->GetChildren()) {
        if (auto* ve = dynamic_cast<VisualElement*>(child))
            rootChildren.push_back(ve);
    }
    std::stable_sort(rootChildren.begin(), rootChildren.end(),
                     [](const VisualElement* a, const VisualElement* b) {
                         return a->zOrder < b->zOrder;
                     });

    // Once Visible, `timer` is reused as the duration/auto-hide countdown
    // clock (see Tick()), not in-animation progress — render the
    // fully-settled pose instead of re-evaluating inAnimation from t=0, which
    // would visibly replay the transition for `duration` seconds every time a
    // timed title becomes Visible. EvaluateAnimation clamps its internal
    // progress ratio to [0, 1], so any sufficiently large `t` settles every
    // element's (and every descendant's) animation regardless of its own
    // delay/duration — far larger than any realistic animation length.
    constexpr double kSettledTimer = 1e6;

    for (VisualElement* ve : rootChildren) {
        const auto& def = isOut ? ve->outAnimation : ve->inAnimation;
        double t = isVisible ? kSettledTimer : timer;
        AnimatedTransform xf = animation::EvaluateAnimation(
            def, t, isOut, ve->GetSize().width, ve->GetSize().height);

        Point pos = ve->GetGlobalPosition();
        cairo_save(ctx);
        cairo_translate(ctx, pos.x, pos.y);
        ve->Render(ctx, xf, t, isOut);
        cairo_restore(ctx);
    }
}

void Title::Render(cairo_t* ctx) const
{
    if (state == TitleState::Hidden) return;
    RenderElements(ctx);
}

void Title::Render(cairo_t* ctx, int outputWidth, int outputHeight) const
{
    if (state == TitleState::Hidden) return;

    cairo_save(ctx);
    cairo_scale(ctx,
                static_cast<double>(outputWidth)  / width,
                static_cast<double>(outputHeight) / height);
    RenderElements(ctx);
    cairo_restore(ctx);
}

// ── Parsers (shared with Load) ────────────────────────────────────────────────

static Easing ParseEasing(const std::string& s)
{
    if (s == "ease_in")              return Easing::EaseIn;
    if (s == "ease_out")             return Easing::EaseOut;
    if (s == "ease_in_out")          return Easing::EaseInOut;
    if (s == "ease_in_cubic")        return Easing::EaseInCubic;
    if (s == "ease_out_cubic")       return Easing::EaseOutCubic;
    if (s == "ease_in_out_cubic")    return Easing::EaseInOutCubic;
    if (s == "ease_in_expo")         return Easing::EaseInExpo;
    if (s == "ease_out_expo")        return Easing::EaseOutExpo;
    if (s == "ease_in_out_expo")     return Easing::EaseInOutExpo;
    if (s == "ease_in_back")         return Easing::EaseInBack;
    if (s == "ease_out_back")        return Easing::EaseOutBack;
    if (s == "ease_in_out_back")     return Easing::EaseInOutBack;
    if (s == "ease_in_elastic")      return Easing::EaseInElastic;
    if (s == "ease_out_elastic")     return Easing::EaseOutElastic;
    if (s == "ease_in_out_elastic")  return Easing::EaseInOutElastic;
    return Easing::Linear;
}

static AnimationType ParseAnimationType(const std::string& s)
{
    if (s == "fade")        return AnimationType::Fade;
    if (s == "slide_up")    return AnimationType::SlideUp;
    if (s == "slide_down")  return AnimationType::SlideDown;
    if (s == "slide_left")  return AnimationType::SlideLeft;
    if (s == "slide_right") return AnimationType::SlideRight;
    if (s == "scale_in")    return AnimationType::ScaleIn;
    if (s == "wipe_up")     return AnimationType::WipeUp;
    if (s == "wipe_down")   return AnimationType::WipeDown;
    if (s == "wipe_left")   return AnimationType::WipeLeft;
    if (s == "wipe_right")  return AnimationType::WipeRight;
    return AnimationType::None;
}

static AnimationDef ParseAnimationDef(const json& j)
{
    return {
        .type     = ParseAnimationType(j.value("type", "none")),
        .easing   = ParseEasing(j.value("easing", "linear")),
        .duration = j.value("duration", 0.5f),
        .delay    = j.value("delay", 0.0f),
    };
}

static FontWeight ParseFontWeight(const std::string& s)
{
    if (s == "thin")        return FontWeight::Thin;
    if (s == "ultrathin")   return FontWeight::UltraThin;
    if (s == "ultralight")  return FontWeight::UltraLight;
    if (s == "semilight")   return FontWeight::SemiLight;
    if (s == "light")       return FontWeight::Light;
    if (s == "book")        return FontWeight::Book;
    if (s == "medium")      return FontWeight::Medium;
    if (s == "semibold")    return FontWeight::SemiBold;
    if (s == "bold")        return FontWeight::Bold;
    if (s == "ultrabold")   return FontWeight::UltraBold;
    if (s == "heavy")       return FontWeight::Heavy;
    if (s == "ultraheavy")  return FontWeight::UltraHeavy;
    return FontWeight::Normal;
}

static HorizontalAlignment ParseHAlignment(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "center")                           return HorizontalAlignment::Center;
    if (s == "far" || s == "right" || s == "end") return HorizontalAlignment::Right;
    if (s == "justify")                          return HorizontalAlignment::Justify;
    return HorizontalAlignment::Left;
}

static VerticalAlignment ParseVAlignment(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "bottom" || s == "end" || s == "down") return VerticalAlignment::Bottom;
    if (s == "center" || s == "middle")             return VerticalAlignment::Middle;
    return VerticalAlignment::Top;
}

static Ellipsize ParseEllipsize(const std::string& s)
{
    if (s == "start")  return Ellipsize::Start;
    if (s == "middle") return Ellipsize::Middle;
    if (s == "end")    return Ellipsize::End;
    return Ellipsize::None;
}

static WrapMode ParseWrapMode(const std::string& s)
{
    if (s == "char")       return WrapMode::Char;
    if (s == "word_char")  return WrapMode::WordChar;
    return WrapMode::Word;
}

static TextTransform ParseTextTransform(const std::string& s)
{
    if (s == "capitalize") return TextTransform::Capitalize;
    if (s == "uppercase")  return TextTransform::Uppercase;
    if (s == "lowercase")  return TextTransform::Lowercase;
    return TextTransform::None;
}

static ScaleMode ParseScaleMode(const std::string& s)
{
    if (s == "contain")    return ScaleMode::Contain;
    if (s == "cover")      return ScaleMode::Cover;
    if (s == "fit_width")  return ScaleMode::FitWidth;
    if (s == "fit_height") return ScaleMode::FitHeight;
    if (s == "none")       return ScaleMode::None;
    if (s == "tile")       return ScaleMode::Tile;
    return ScaleMode::Stretch;
}

static Paint ParsePaint(const json& v)
{
    if (v.is_array() && v.size() >= 3) {
        double r = v[0], g = v[1], b = v[2];
        double a = v.size() >= 4 ? (double)v[3] : 1.0;
        return Paint::Solid(r, g, b, a);
    }

    if (v.is_string())
        return Paint::Image(v.get<std::string>());

    if (v.is_object()) {
        std::string type = v.value("type", "");

        if (type == "image") {
            Paint p = Paint::Image(v.value("path", ""));
            p.imageScaleMode = ParseScaleMode(v.value("scale_mode", "stretch"));
            p.tileScale = v.value("tile_scale", 1.0);
            return p;
        }

        Paint p = (type == "radial")
                      ? Paint::Radial(v.value("cx0", 0.0), v.value("cy0", 0.0), v.value("r0", 0.0),
                                      v.value("cx1", 0.0), v.value("cy1", 0.0), v.value("r1", 1.0))
                      : Paint::Linear(v.value("x0", 0.0), v.value("y0", 0.0), v.value("x1", 1.0),
                                      v.value("y1", 0.0));

        if (v.contains("stops") && v["stops"].is_array()) {
            for (const auto& s : v["stops"]) {
                double offset = s.value("offset", 0.0);
                const auto& c = s.at("color");
                double r = c[0], g = c[1], b = c[2];
                double a = c.size() >= 4 ? (double)c[3] : 1.0;
                p.AddStop(offset, r, g, b, a);
            }
        }
        return p;
    }

    return Paint{};
}

// Element JSON keys consumed below (common to every element type). Keep in
// sync with ParseCommonProperties. Does NOT include "type" or "parent" —
// those are consumed one level up (ParseElement / Title::Load) — nor any
// type-specific keys, which each ParseElement branch tracks separately.
static const std::unordered_set<std::string> kCommonElementKeys = {
    "id", "x", "y", "w", "h", "fill", "stroke", "color", "corner_radius",
    "z_order", "stroke_width", "opacity", "rotation", "shear_x", "shear_y",
    "shadow", "anim_in", "anim_out", "data_anim_in", "data_anim_out",
    "clip_children", "fit_to_children", "children_padding",
};

// Copies every key in `j` that isn't a recognized common/type-specific/
// structural key into el.extra, so it survives a Load->Save round-trip even
// though nothing in the engine currently reads it. `typeKeys` is the set of
// keys the calling ParseElement branch consumes beyond the common ones.
static void CaptureExtraElementKeys(VisualElement& el, const json& j,
                                     const std::unordered_set<std::string>& typeKeys)
{
    for (const auto& [k, v] : j.items()) {
        if (k == "type" || k == "parent") continue;
        if (kCommonElementKeys.count(k)) continue;
        if (typeKeys.count(k)) continue;
        el.extra[k] = v;
    }
}

static void ParseCommonProperties(VisualElement& el, const json& j)
{
    el.SetId(j.value("id", ""));
    el.SetBounds({j.value("x", 0.0), j.value("y", 0.0), j.value("w", 0.0), j.value("h", 0.0)});

    if (j.contains("fill"))   el.fill   = ParsePaint(j["fill"]);
    if (j.contains("stroke")) el.stroke = ParsePaint(j["stroke"]);
    if (j.contains("color"))  el.fill   = ParsePaint(j["color"]);

    if (j.contains("corner_radius")) {
        const auto& cr = j["corner_radius"];
        if (cr.is_number()) {
            float r = cr.get<float>();
            el.cornerRadius[0] = el.cornerRadius[1] = el.cornerRadius[2] = el.cornerRadius[3] = r;
        } else if (cr.is_array() && cr.size() >= 4) {
            for (int i = 0; i < 4; i++) el.cornerRadius[i] = cr[i];
        }
    }

    el.zOrder      = j.value("z_order", 0);
    el.strokeWidth = j.value("stroke_width", 0.0f);
    el.opacity     = j.value("opacity", 1.0f);
    el.SetRotation(j.value("rotation", 0.0f));
    el.SetShearX(j.value("shear_x", 0.0f));
    el.SetShearY(j.value("shear_y", 0.0f));

    if (j.contains("shadow") && j["shadow"].is_object()) {
        const auto& sj = j["shadow"];
        el.shadow.enabled  = sj.value("enabled", true);
        el.shadow.offsetX  = sj.value("offset_x", 4.0);
        el.shadow.offsetY  = sj.value("offset_y", 4.0);
        el.shadow.blur     = sj.value("blur", 8.0);
        if (sj.contains("color") && sj["color"].is_array() && sj["color"].size() >= 4)
            for (int i = 0; i < 4; ++i) el.shadow.color[i] = sj["color"][i].get<float>();
    }

    if (j.contains("anim_in"))       el.inAnimation      = ParseAnimationDef(j["anim_in"]);
    if (j.contains("anim_out"))      el.outAnimation     = ParseAnimationDef(j["anim_out"]);
    if (j.contains("data_anim_in"))  el.dataInAnimation  = ParseAnimationDef(j["data_anim_in"]);
    if (j.contains("data_anim_out")) el.dataOutAnimation = ParseAnimationDef(j["data_anim_out"]);

    el.clipChildren  = j.value("clip_children", false);
    el.fitToChildren = j.value("fit_to_children", false);
    if (j.contains("children_padding") && j["children_padding"].is_array() &&
        j["children_padding"].size() >= 4)
        for (int i = 0; i < 4; ++i)
            el.childrenPadding[i] = j["children_padding"][i].get<float>();
}

static std::unique_ptr<VisualElement> ParseElement(const json& j)
{
    std::string t = j.value("type", "");

    if (t == "text") {
        static const std::unordered_set<std::string> kTextKeys = {
            "text", "font_family", "font_size", "font_italic", "font_underline",
            "font_strikethrough", "font_weight", "auto_scale", "text_align_x",
            "text_align_y", "ellipsize", "wrap", "text_transform",
            "line_spacing", "letter_spacing",
        };
        auto el = std::make_unique<TextElement>();
        el->text                  = j.value("text", "");
        el->font.family           = j.value("font_family", "Sans");
        el->font.size             = j.value("font_size", 36.0f);
        el->font.isItalic         = j.value("font_italic", false);
        el->font.isUnderline      = j.value("font_underline", false);
        el->font.isStrikethrough  = j.value("font_strikethrough", false);
        el->font.weight           = ParseFontWeight(j.value("font_weight", "normal"));
        el->textStyle.autoScale   = j.value("auto_scale", false);
        el->textStyle.alignX      = ParseHAlignment(j.value("text_align_x", "left"));
        el->textStyle.alignY      = ParseVAlignment(j.value("text_align_y", "top"));
        el->textStyle.ellipsize   = ParseEllipsize(j.value("ellipsize", "none"));
        el->textStyle.wrapMode    = ParseWrapMode(j.value("wrap", "word"));
        el->textStyle.transform     = ParseTextTransform(j.value("text_transform", "none"));
        el->textStyle.lineSpacing   = j.value("line_spacing", 0.0f);
        el->textStyle.letterSpacing = j.value("letter_spacing", 0.0f);
        ParseCommonProperties(*el, j);
        CaptureExtraElementKeys(*el, j, kTextKeys);
        return el;
    }

    if (t == "image") {
        static const std::unordered_set<std::string> kImageKeys = {
            "image_path", "scale_mode", "image_tile_scale",
        };
        auto el = std::make_unique<ImageElement>();
        el->imagePath      = j.value("image_path", "");
        el->imageScaleMode = ParseScaleMode(j.value("scale_mode", "stretch"));
        el->imageTileScale = j.value("image_tile_scale", 1.0);
        ParseCommonProperties(*el, j);
        CaptureExtraElementKeys(*el, j, kImageKeys);
        return el;
    }

    if (t == "qr_code") {
        static const std::unordered_set<std::string> kQrKeys = {"text"};
        auto el = std::make_unique<QrElement>();
        el->text = j.value("text", "");
        ParseCommonProperties(*el, j);
        CaptureExtraElementKeys(*el, j, kQrKeys);
        return el;
    }

    auto el = std::make_unique<RectangleElement>();
    ParseCommonProperties(*el, j);
    CaptureExtraElementKeys(*el, j, {});
    return el;
}

// ── Serialisers ───────────────────────────────────────────────────────────────

static const char* EasingToStr(Easing e)
{
    switch (e) {
    case Easing::EaseIn:            return "ease_in";
    case Easing::EaseOut:           return "ease_out";
    case Easing::EaseInOut:         return "ease_in_out";
    case Easing::EaseInCubic:       return "ease_in_cubic";
    case Easing::EaseOutCubic:      return "ease_out_cubic";
    case Easing::EaseInOutCubic:    return "ease_in_out_cubic";
    case Easing::EaseInExpo:        return "ease_in_expo";
    case Easing::EaseOutExpo:       return "ease_out_expo";
    case Easing::EaseInOutExpo:     return "ease_in_out_expo";
    case Easing::EaseInBack:        return "ease_in_back";
    case Easing::EaseOutBack:       return "ease_out_back";
    case Easing::EaseInOutBack:     return "ease_in_out_back";
    case Easing::EaseInElastic:     return "ease_in_elastic";
    case Easing::EaseOutElastic:    return "ease_out_elastic";
    case Easing::EaseInOutElastic:  return "ease_in_out_elastic";
    default:                        return "linear";
    }
}

static const char* AnimTypeToStr(AnimationType t)
{
    switch (t) {
    case AnimationType::Fade:       return "fade";
    case AnimationType::SlideUp:    return "slide_up";
    case AnimationType::SlideDown:  return "slide_down";
    case AnimationType::SlideLeft:  return "slide_left";
    case AnimationType::SlideRight: return "slide_right";
    case AnimationType::ScaleIn:    return "scale_in";
    case AnimationType::WipeUp:     return "wipe_up";
    case AnimationType::WipeDown:   return "wipe_down";
    case AnimationType::WipeLeft:   return "wipe_left";
    case AnimationType::WipeRight:  return "wipe_right";
    default:                        return "none";
    }
}

static const char* ScaleModeToStr(ScaleMode m)
{
    switch (m) {
    case ScaleMode::Contain:   return "contain";
    case ScaleMode::Cover:     return "cover";
    case ScaleMode::FitWidth:  return "fit_width";
    case ScaleMode::FitHeight: return "fit_height";
    case ScaleMode::None:      return "none";
    case ScaleMode::Tile:      return "tile";
    default:                   return "stretch";
    }
}

static const char* FontWeightToStr(FontWeight w)
{
    // UltraThin == Thin (both map to PANGO_WEIGHT_THIN); only one case allowed
    switch (w) {
    case FontWeight::Thin:       return "thin";
    case FontWeight::UltraLight: return "ultralight";
    case FontWeight::SemiLight:  return "semilight";
    case FontWeight::Light:      return "light";
    case FontWeight::Book:       return "book";
    case FontWeight::Medium:     return "medium";
    case FontWeight::SemiBold:   return "semibold";
    case FontWeight::Bold:       return "bold";
    case FontWeight::UltraBold:  return "ultrabold";
    case FontWeight::Heavy:      return "heavy";
    case FontWeight::UltraHeavy: return "ultraheavy";
    default:                     return "normal";
    }
}

static json SerializeAnimDef(const AnimationDef& def)
{
    if (def.type == AnimationType::None) return nullptr;
    return json{
        {"type",     AnimTypeToStr(def.type)},
        {"easing",   EasingToStr(def.easing)},
        {"duration", def.duration},
        {"delay",    def.delay},
    };
}

using AssetRegFn = std::function<std::string(const std::string&)>;

static json SerializePaint(const Paint& p, const AssetRegFn& registerAsset)
{
    switch (p.type) {
    case Paint::Type::Solid:
        return json::array({p.params[0], p.params[1], p.params[2], p.params[3]});

    case Paint::Type::Image: {
        std::string atPath = p.imagePath.empty() ? "" : registerAsset(p.imagePath);
        json j;
        j["type"] = "image";
        j["path"] = atPath;
        if (p.imageScaleMode != ScaleMode::Stretch) j["scale_mode"] = ScaleModeToStr(p.imageScaleMode);
        if (p.tileScale != 1.0)                     j["tile_scale"] = p.tileScale;
        return j;
    }

    case Paint::Type::Linear: {
        json j;
        j["type"] = "linear";
        j["x0"] = p.params[0]; j["y0"] = p.params[1];
        j["x1"] = p.params[2]; j["y1"] = p.params[3];
        json stops = json::array();
        for (const auto& s : p.stops)
            stops.push_back({{"offset", s.offset}, {"color", json::array({s.r, s.g, s.b, s.a})}});
        j["stops"] = stops;
        return j;
    }

    case Paint::Type::Radial: {
        json j;
        j["type"] = "radial";
        j["cx0"] = p.params[0]; j["cy0"] = p.params[1]; j["r0"] = p.params[2];
        j["cx1"] = p.params[3]; j["cy1"] = p.params[4]; j["r1"] = p.params[5];
        json stops = json::array();
        for (const auto& s : p.stops)
            stops.push_back({{"offset", s.offset}, {"color", json::array({s.r, s.g, s.b, s.a})}});
        j["stops"] = stops;
        return j;
    }

    default:
        return nullptr;
    }
}

static json SerializeElement(const IElement* iel, const IElement* root, const AssetRegFn& registerAsset)
{
    const auto* el = static_cast<const VisualElement*>(iel);
    json j;

    j["id"] = el->GetId();
    auto pos = el->GetPosition();
    auto sz  = el->GetSize();
    j["x"] = pos.x; j["y"] = pos.y;
    j["w"] = sz.width; j["h"] = sz.height;

    // parent — omit for direct children of root
    if (el->GetParent() && el->GetParent() != root)
        j["parent"] = el->GetParent()->GetId();

    if (el->clipChildren) j["clip_children"] = true;

    auto fillJ = SerializePaint(el->fill, registerAsset);
    if (!fillJ.is_null()) j["fill"] = fillJ;
    auto strokeJ = SerializePaint(el->stroke, registerAsset);
    if (!strokeJ.is_null()) j["stroke"] = strokeJ;

    bool allSame = el->cornerRadius[0] == el->cornerRadius[1] &&
                   el->cornerRadius[0] == el->cornerRadius[2] &&
                   el->cornerRadius[0] == el->cornerRadius[3];
    if (el->cornerRadius[0] > 0.0f || !allSame) {
        if (allSame) j["corner_radius"] = el->cornerRadius[0];
        else j["corner_radius"] = json::array({el->cornerRadius[0], el->cornerRadius[1],
                                                el->cornerRadius[2], el->cornerRadius[3]});
    }

    if (el->zOrder != 0)         j["z_order"]      = el->zOrder;
    if (el->strokeWidth > 0.0f)  j["stroke_width"]  = el->strokeWidth;
    if (el->opacity != 1.0f)     j["opacity"]       = el->opacity;
    if (el->GetRotation() != 0.0f) j["rotation"]    = el->GetRotation();
    if (el->GetShearX() != 0.0f)   j["shear_x"]     = el->GetShearX();
    if (el->GetShearY() != 0.0f)   j["shear_y"]     = el->GetShearY();

    if (el->shadow.enabled) {
        j["shadow"] = {
            {"enabled",  true},
            {"offset_x", el->shadow.offsetX},
            {"offset_y", el->shadow.offsetY},
            {"blur",     el->shadow.blur},
            {"color",    json::array({el->shadow.color[0], el->shadow.color[1],
                                      el->shadow.color[2], el->shadow.color[3]})},
        };
    }

    auto animIn   = SerializeAnimDef(el->inAnimation);
    auto animOut  = SerializeAnimDef(el->outAnimation);
    auto dataIn   = SerializeAnimDef(el->dataInAnimation);
    auto dataOut  = SerializeAnimDef(el->dataOutAnimation);
    if (!animIn.is_null())  j["anim_in"]       = animIn;
    if (!animOut.is_null()) j["anim_out"]      = animOut;
    if (!dataIn.is_null())  j["data_anim_in"]  = dataIn;
    if (!dataOut.is_null()) j["data_anim_out"] = dataOut;

    if (el->fitToChildren) {
        j["fit_to_children"] = true;
        bool anyPad = false;
        for (int i = 0; i < 4; i++) if (el->childrenPadding[i] != 0.0f) anyPad = true;
        if (anyPad)
            j["children_padding"] = json::array({el->childrenPadding[0], el->childrenPadding[1],
                                                  el->childrenPadding[2], el->childrenPadding[3]});
    }

    // type-specific fields
    if (const auto* te = dynamic_cast<const TextElement*>(el)) {
        j["type"] = "text";
        j["text"] = te->text;
        j["font_family"]      = te->font.family;
        j["font_size"]        = te->font.size;
        j["font_weight"]      = FontWeightToStr(te->font.weight);
        if (te->font.isItalic)        j["font_italic"]        = true;
        if (te->font.isUnderline)     j["font_underline"]     = true;
        if (te->font.isStrikethrough) j["font_strikethrough"] = true;
        if (te->textStyle.autoScale)  j["auto_scale"]         = true;

        if (te->textStyle.alignX != HorizontalAlignment::Left) {
            switch (te->textStyle.alignX) {
            case HorizontalAlignment::Center:  j["text_align_x"] = "center";  break;
            case HorizontalAlignment::Right:   j["text_align_x"] = "right";   break;
            case HorizontalAlignment::Justify: j["text_align_x"] = "justify"; break;
            default: break;
            }
        }
        if (te->textStyle.alignY != VerticalAlignment::Top) {
            switch (te->textStyle.alignY) {
            case VerticalAlignment::Middle: j["text_align_y"] = "middle"; break;
            case VerticalAlignment::Bottom: j["text_align_y"] = "bottom"; break;
            default: break;
            }
        }
        if (te->textStyle.ellipsize != Ellipsize::None) {
            switch (te->textStyle.ellipsize) {
            case Ellipsize::Start:  j["ellipsize"] = "start";  break;
            case Ellipsize::Middle: j["ellipsize"] = "middle"; break;
            case Ellipsize::End:    j["ellipsize"] = "end";    break;
            default: break;
            }
        }
        if (te->textStyle.wrapMode != WrapMode::Word) {
            switch (te->textStyle.wrapMode) {
            case WrapMode::Char:     j["wrap"] = "char";      break;
            case WrapMode::WordChar: j["wrap"] = "word_char"; break;
            default: break;
            }
        }
        if (te->textStyle.transform != TextTransform::None) {
            switch (te->textStyle.transform) {
            case TextTransform::Capitalize: j["text_transform"] = "capitalize"; break;
            case TextTransform::Uppercase:  j["text_transform"] = "uppercase";  break;
            case TextTransform::Lowercase:  j["text_transform"] = "lowercase";  break;
            default: break;
            }
        }
        if (te->textStyle.lineSpacing   != 0.0f) j["line_spacing"]   = te->textStyle.lineSpacing;
        if (te->textStyle.letterSpacing != 0.0f) j["letter_spacing"] = te->textStyle.letterSpacing;
    } else if (const auto* ie = dynamic_cast<const ImageElement*>(el)) {
        j["type"] = "image";
        if (!ie->imagePath.empty()) j["image_path"] = registerAsset(ie->imagePath);
        if (ie->imageScaleMode != ScaleMode::Stretch) j["scale_mode"] = ScaleModeToStr(ie->imageScaleMode);
        if (ie->imageTileScale != 1.0) j["image_tile_scale"] = ie->imageTileScale;
    } else if (const auto* qe = dynamic_cast<const QrElement*>(el)) {
        j["type"] = "qr_code";
        j["text"] = qe->text;
    } else {
        j["type"] = "rectangle";
    }

    // Merge back any keys the parser didn't recognize on load (forward-compat
    // for a newer plugin's fields). Known/typed keys always win.
    for (const auto& [k, v] : el->extra.items()) {
        if (!j.contains(k)) j[k] = v;
    }

    return j;
}

namespace ogt {
std::unique_ptr<VisualElement> ParseElement(const nlohmann::json& j) {
    return ::ParseElement(j);
}
nlohmann::json SerializeElement(const IElement* el, const IElement* root) {
    return ::SerializeElement(el, root, [](const std::string& p) { return p; });
}
}

// ── Schema migrations ────────────────────────────────────────────────────────
//
// Ordered registry of migration steps applied to a title.json's in-memory
// `json` value right after parsing/version-resolution and before any field
// extraction. Each step names the schema version it upgrades *from* and a
// callback that mutates `j` in place so it looks like a file written at the
// next schema version.
//
// The (0,0) step is a deliberate no-op: schema 1.0 only *added* the
// "schema_version" (and legacy "version") field on top of the unversioned
// pre-E4 baseline, and absent fields already degrade safely via
// `j.value(key, default)` at the call sites below. The (1,0) step is the
// first real one — see its comment.
struct SchemaMigrationStep {
    int major;
    int minor;
    std::function<void(json&)> apply;
};

static const std::vector<SchemaMigrationStep>& MigrationRegistry()
{
    static const std::vector<SchemaMigrationStep> steps = {
        // (0,0) -> (1,0): pre-E4 / legacy unversioned files. Nothing to
        // transform — schema 1.0 is additive-only relative to (0,0).
        {0, 0, [](json& /*j*/) {
            // Intentional no-op: no field was renamed/removed going into
            // schema 1.0, so there is nothing to migrate here today.
        }},
        // (1,0) -> (1,1): "id" changed meaning. It used to hold the
        // human-readable name ("lower_third"); it now holds the title's
        // generated uuid, and the name moved to "name". Detect a pre-1.1
        // file by the shape of "id" — anything that isn't a v4 uuid is a
        // legacy name — so this stays correct even if the step is somehow
        // applied twice.
        {1, 0, [](json& j) {
            const bool legacyId = j.contains("id") && j["id"].is_string() &&
                                  !uuid::IsV4(j["id"].get<std::string>());
            if (legacyId && !j.contains("name"))
                j["name"] = j["id"];
            if (legacyId || !j.contains("id") || !j["id"].is_string())
                j["id"] = uuid::GenerateV4();
        }},
        // Extension point: when a future schema bump introduces a real
        // breaking change, add a new ordered step here. `j` is the WHOLE
        // title.json object, so a step operates on top-level title keys
        // and/or walks j["elements"] (recursing into nested children) for
        // element-field changes — the far more common case. e.g. renaming
        // the per-element "stroke_width" number into a nested
        // "stroke": {"width": ...} object:
        //
        // {1, 0, [](json& j) {
        //     if (!j.contains("elements") || !j["elements"].is_array()) return;
        //     std::function<void(json&)> fix = [&](json& el) {
        //         if (el.contains("stroke_width")) {
        //             el["stroke"]["width"] = el["stroke_width"];
        //             el.erase("stroke_width");
        //         }
        //         if (el.contains("children") && el["children"].is_array())
        //             for (auto& c : el["children"]) fix(c);
        //     };
        //     for (auto& el : j["elements"]) fix(el);
        // }},
    };
    return steps;
}

static bool SchemaVersionLess(int aMajor, int aMinor, int bMajor, int bMinor)
{
    return std::make_pair(aMajor, aMinor) < std::make_pair(bMajor, bMinor);
}

// Not `static`: kept internal (no title.h declaration, not part of the
// public API) but with external linkage so a standalone test binary can
// forward-declare and link against it directly, mirroring ClassifySchema
// above. Applies every registered step whose source version is >= the
// file's resolved (fromMajor, fromMinor) and < the schema this build
// understands (kOgtSchemaMajor.kOgtSchemaMinor), in registration order.
void MigrateTitleJson(nlohmann::json& j, int fromMajor, int fromMinor)
{
    for (const auto& step : MigrationRegistry()) {
        const bool stepAtOrAfterFile =
            !SchemaVersionLess(step.major, step.minor, fromMajor, fromMinor);
        const bool stepBeforeCurrent =
            SchemaVersionLess(step.major, step.minor, kOgtSchemaMajor, kOgtSchemaMinor);
        if (stepAtOrAfterFile && stepBeforeCurrent)
            step.apply(j);
    }
}

// ── Load ──────────────────────────────────────────────────────────────────────

// Replace '@name' strings in JSON with their resolved temp-dir paths
static void ReplaceAtPaths(json& j, const std::unordered_map<std::string, std::string>& pathMap)
{
    if (j.is_string()) {
        auto it = pathMap.find(j.get<std::string>());
        if (it != pathMap.end()) j = it->second;
    } else if (j.is_object()) {
        for (auto& [k, v] : j.items()) ReplaceAtPaths(v, pathMap);
    } else if (j.is_array()) {
        for (auto& v : j) ReplaceAtPaths(v, pathMap);
    }
}

Title Title::Load(const std::string& ogtPath)
{
    std::string jsonStr;
    std::string tempDir;
    std::unordered_map<std::string, std::string> pathMap;  // "@name" → "/tmp/.../name"
    std::vector<uint8_t> thumbnail;

    zipper::enumerate(ogtPath, [&](zipper::UnZip& uz) {
        if (uz.is_dir()) return;
        std::string entry = uz.file_path();

        if (entry == "title.json") {
            uz.read(jsonStr);
        } else if (entry.rfind("ASSETS/", 0) == 0) {
            std::string baseName = entry.substr(7);
            if (tempDir.empty()) {
                tempDir = (fs::temp_directory_path() /
                           ("ogt-" + fs::path(ogtPath).stem().string() + "-" +
                            std::to_string(std::hash<std::string>{}(ogtPath) & 0xFFFF))).string();
                fs::create_directories(tempDir);
            }
            std::string tempPath = tempDir + "/" + baseName;
            std::string buf;
            if (uz.read(buf)) {
                std::ofstream f(tempPath, std::ios::binary);
                f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            }
            pathMap["@" + baseName] = tempPath;
        } else if (entry == "thumb.png") {
            std::string buf;
            uz.read(buf);
            thumbnail.assign(buf.begin(), buf.end());
        }
    });

    if (jsonStr.empty())
        throw std::runtime_error("Missing title.json in " + ogtPath);

    // Parse JSON and resolve @-paths
    json j = json::parse(jsonStr);
    if (!pathMap.empty()) ReplaceAtPaths(j, pathMap);

    // Resolve the file's (major, minor) schema version. Prefer the new
    // structured "schema_version" object; fall back to the legacy int
    // "version" field; default to {0, 0} (pre-E4 / absent) otherwise.
    //
    // Guard every read with an explicit type check rather than relying on
    // json::value()'s default: value() only substitutes the default when
    // the key is *absent*, but still throws type_error.302 if the key is
    // *present with the wrong type* (e.g. a hand-edited/corrupt file with
    // "major": "1"). A malformed version node must degrade to the
    // default, not throw.
    int fileMajor = 0;
    int fileMinor = 0;
    if (j.contains("schema_version") && j["schema_version"].is_object()) {
        const json& sv = j["schema_version"];
        if (sv.contains("major") && sv["major"].is_number_integer())
            fileMajor = sv["major"].get<int>();
        if (sv.contains("minor") && sv["minor"].is_number_integer())
            fileMinor = sv["minor"].get<int>();
    } else if (j.contains("version") && j["version"].is_number_integer()) {
        fileMajor = j.value("version", 0);
        fileMinor = 0;
    }

    // Run schema migrations now that (fileMajor, fileMinor) is resolved and
    // before any field extraction below consumes `j`. See MigrationRegistry
    // above for the (currently no-op) step list and extension pattern.
    MigrateTitleJson(j, fileMajor, fileMinor);

    // Structured diagnostic for the caller (e.g. editor UI), replacing the
    // old stderr-only warning. Populated here, applied to `title` below once
    // it's constructed.
    Title::LoadDiagnostic diag;
    switch (ClassifySchema(fileMajor, fileMinor)) {
        case SchemaCompat::Ok:
            // Current or older-minor within the same major: load silently.
            break;
        case SchemaCompat::OlderMigrate:
            // Older major. No migrations implemented yet (see TODO above);
            // load as-is for now. A successful migrate is not a user-facing
            // warning.
            break;
        case SchemaCompat::NewerMinor:
            diag.severity = Title::LoadDiagnostic::Severity::Info;
            diag.message =
                "This file was created with a newer minor version of the schema (file " +
                std::to_string(fileMajor) + "." + std::to_string(fileMinor) + ", editor " +
                std::to_string(kOgtSchemaMajor) + "." + std::to_string(kOgtSchemaMinor) +
                "). Unknown fields are preserved on save.";
            break;
        case SchemaCompat::NewerMajor:
            // Major version ahead of what we support: fields may have been
            // repurposed or removed, not just added on top. We still load
            // (best-effort/degraded) rather than reject outright, because
            // unknown-key preservation (title.extra) means we don't lose
            // data on round-trip. A future major bump MAY choose to
            // hard-reject here instead if degraded loads prove unsafe.
            diag.severity = Title::LoadDiagnostic::Severity::Warning;
            diag.message =
                "This file was created with a newer major version of the schema (file " +
                std::to_string(fileMajor) + "." + std::to_string(fileMinor) + ", editor " +
                std::to_string(kOgtSchemaMajor) + "." + std::to_string(kOgtSchemaMinor) +
                "). It may not display correctly; unknown fields are preserved but new features "
                "are ignored.";
            break;
    }

    Title title;
    title.m_loadDiagnostic = diag;
    // Pre-1.1 files are normalized by the migration step above, so "id" is a
    // uuid and "name" the human name by the time we get here. The fallback
    // still guards a hand-edited/blank id.
    title.id       = j.value("id", "");
    if (title.id.empty()) title.id = uuid::GenerateV4();
    title.name     = j.value("name", "");
    title.width    = j.value("width", 1920);
    title.height   = j.value("height", 1080);
    title.metadata = j.value("metadata", nlohmann::json::object());
    title.m_tempAssetDir = tempDir;
    title.m_thumbnail    = std::move(thumbnail);

    // Top-level title.json keys consumed above (+ "elements", parsed below).
    // Keep in sync with the reads above. Anything else — e.g. a newer
    // plugin's field — is preserved in title.extra for round-tripping.
    static const std::unordered_set<std::string> kKnownTitleKeys = {
        "id", "name", "version", "schema_version", "width", "height", "metadata", "elements",
    };
    for (const auto& [k, v] : j.items()) {
        if (!kKnownTitleKeys.count(k))
            title.extra[k] = v;
    }

    // Parse elements
    struct PendingRef { size_t idx; std::string parentId; };
    std::vector<PendingRef> pending;

    if (j.contains("elements") && j["elements"].is_array()) {
        for (const auto& ej : j["elements"]) {
            pending.push_back({.idx      = title.elements.size(),
                               .parentId = ej.value("parent", "")});
            title.elements.push_back(ParseElement(ej));
        }
    }

    // Build id → index map (root is elements[0])
    std::unordered_map<std::string, size_t> idMap;
    for (size_t i = 0; i < title.elements.size(); ++i)
        idMap[title.elements[i]->GetId()] = i;

    IElement* root = title.GetRoot();

    // Resolve mask and parent references
    for (auto& ref : pending) {
        auto& el = *static_cast<VisualElement*>(title.elements[ref.idx].get());

        if (!ref.parentId.empty()) {
            auto it = idMap.find(ref.parentId);
            if (it != idMap.end()) {
                IElement* parent = title.elements[it->second].get();
                // JSON x/y are local; AddChild→SetParent preserves world, so convert first
                Point childLocal = el.GetPosition();
                Point parentWorld = parent->GetGlobalPosition();
                el.SetPosition({childLocal.x + parentWorld.x, childLocal.y + parentWorld.y});
                parent->AddChild(&el);
            }
        } else {
            // No parent field → parent is root (root is at origin, no conversion needed)
            root->AddChild(&el);
        }
    }

    return title;
}

// ── Save ──────────────────────────────────────────────────────────────────────

void Title::Save(const std::string& ogtPath) const
{
    // Track assets to bundle: resolved local path → "@basename" (bundled name)
    std::unordered_map<std::string, std::string> assetMap;
    // resolvedPath → assigned "@name" (for stability across repeated registrations
    // of the same path), and the set of base names already claimed (to disambiguate
    // collisions between distinct paths that share a filename).
    std::unordered_map<std::string, std::string> pathToAt;
    std::unordered_set<std::string> usedNames;

    auto registerAsset = [&](const std::string& path) -> std::string {
        if (path.empty()) return path;

        std::string resolvedPath;
        std::string baseName;

        if (path[0] == '@') {
            baseName     = path.substr(1);
            resolvedPath = m_tempAssetDir + "/" + baseName;
        } else {
            resolvedPath = path;
            baseName     = fs::path(path).filename().string();
        }

        auto existing = pathToAt.find(resolvedPath);
        if (existing != pathToAt.end())
            return existing->second;

        std::string uniqueName = baseName;
        if (usedNames.count(uniqueName)) {
            fs::path p(baseName);
            std::string stem = p.stem().string();
            std::string ext  = p.extension().string();
            int counter = 1;
            do {
                uniqueName = stem + "_" + std::to_string(counter) + ext;
                ++counter;
            } while (usedNames.count(uniqueName));
        }

        usedNames.insert(uniqueName);
        std::string atName = "@" + uniqueName;
        pathToAt.emplace(resolvedPath, atName);
        assetMap.emplace(resolvedPath, atName);
        return atName;
    };

    // Build title.json
    json j;
    j["id"]   = id;    // uuid since schema 1.1 (pre-1.1 files stored the name here)
    j["name"] = name;
    // Dual-write the schema version: "schema_version" (major/minor object) is
    // preferred by new readers, while "version" (legacy major-only int) keeps
    // older E4-era readers, which only know that field, working.
    j["schema_version"] = { {"major", kOgtSchemaMajor}, {"minor", kOgtSchemaMinor} };
    j["version"] = kOgtSchemaMajor;
    j["width"]   = width;
    j["height"]  = height;
    if (!metadata.empty()) j["metadata"] = metadata;

    // Merge back any top-level keys the loader didn't recognize (forward-compat
    // for a newer plugin's fields). Known keys always win.
    for (const auto& [k, v] : extra.items()) {
        if (!j.contains(k)) j[k] = v;
    }

    IElement* root = GetRoot();
    json elems = json::array();
    for (size_t i = 1; i < elements.size(); ++i)
        elems.push_back(SerializeElement(elements[i].get(), root, registerAsset));
    j["elements"] = elems;

    // Verify all referenced assets are readable before touching disk, so a
    // missing/unreadable asset never results in a broken .ogt with a title.json
    // that references an asset the archive doesn't contain.
    std::vector<std::string> missing;
    for (const auto& [resolvedPath, atName] : assetMap) {
        std::ifstream f(resolvedPath, std::ios::binary);
        if (!f) missing.push_back(resolvedPath);
    }
    if (!missing.empty()) {
        std::string msg = "Title::Save: cannot bundle missing asset(s): ";
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i) msg += ", ";
            msg += missing[i];
        }
        throw std::runtime_error(msg);
    }

    zipper::Zip zip(ogtPath);
    zip.add_file("title.json", j.dump(2));

    for (const auto& [resolvedPath, atName] : assetMap) {
        std::string baseName = atName.substr(1);
        std::ifstream f(resolvedPath, std::ios::binary);
        if (!f) continue;
        std::string bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>{});
        zip.add_file("ASSETS/" + baseName, bytes);
    }

    if (!m_thumbnail.empty()) {
        std::string thumbStr(m_thumbnail.begin(), m_thumbnail.end());
        zip.add_file("thumb.png", thumbStr);
    }
}
