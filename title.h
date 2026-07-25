// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "data-source.h"
#include "element.h"
#include "visual_element.h"
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

enum class TitleState { Hidden, AnimatingIn, Visible, AnimatingOut };

struct Title {
    // Diagnostic produced by Load() describing schema-version compatibility
    // of the loaded file (if any concern). Reset at the start of every
    // Load(); see GetLoadDiagnostic().
    struct LoadDiagnostic {
        enum class Severity { None, Info, Warning };
        Severity severity = Severity::None;
        std::string message;  // human-readable, empty when severity == None
    };

    std::string id;
    int width{1920}, height{1080};
    nlohmann::json metadata;  // arbitrary editor/consumer data, preserved round-trip
    // Top-level title.json keys the loader doesn't recognize (forward-compat
    // for a newer plugin's fields). Captured on Load, merged back in
    // (without overwriting known fields) on Save. Does not include
    // `metadata`, which already round-trips in full via the field above.
    nlohmann::json extra = nlohmann::json::object();
    std::vector<std::unique_ptr<IElement>> elements;  // [0] is always the auto-created root
    TitleState state{TitleState::Hidden};
    double timer{0.0};
    double duration{-1.0};
    int zOrder{0};

    // Which record (from whatever records a caller last handed in) an
    // element-id lookup indexes into; set by TriggerIn, reused by every
    // subsequent UpdateData call while Visible (see DataPool::Tick).
    size_t dataRecordIndex{0};

    // Fired whenever TriggerIn/TriggerOut runs, regardless of origin (host call,
    // duration timeout, or a script's trigger_out()). Multiple subscribers
    // (e.g. a host UI and a DataPool binding) can listen at once.
    std::vector<std::function<void(size_t recordIndex, double duration)>> onTriggerIn;
    std::vector<std::function<void()>> onTriggerOut;

    Title();
    ~Title();
    Title(const Title&) = delete;
    Title& operator=(const Title&) = delete;
    Title(Title&&) = default;
    Title& operator=(Title&&) = default;

    IElement* GetRoot() const { return elements.empty() ? nullptr : elements[0].get(); }

    // Title never fetches its own data — a caller (typically DataPool)
    // supplies `records` up front. If non-empty, they're applied instantly
    // (SetContentInstant) before the AnimatingIn transition starts; pass an
    // empty vector to trigger in without an instant data change (e.g. a
    // periodic poll will catch up once Visible).
    void TriggerIn(size_t recordIndex = 0, double duration = -1.0, const std::vector<Record>& records = {});
    void TriggerOut();

    // Applies `records[dataRecordIndex % records.size()]` to matching
    // elements by id (SetContentInstant if `instant`, else the animated
    // SetContent). No-op if `records` is empty. Called by TriggerIn (instant
    // apply) and by DataPool::Tick (periodic Visible-only refresh) — Title
    // itself never decides when to poll, it only ever applies what it's handed.
    void UpdateData(const std::vector<Record>& records, bool instant);
    VisualElement& GetById(const std::string& id);

    void Tick(float dt);
    void Render(cairo_t* ctx) const;
    void Render(cairo_t* ctx, int outputWidth, int outputHeight) const;

    static Title Load(const std::string& ogtPath);
    void Save(const std::string& ogtPath) const;

    void SetThumbnail(std::vector<uint8_t> pngBytes) { m_thumbnail = std::move(pngBytes); }
    const std::vector<uint8_t>& GetThumbnail() const { return m_thumbnail; }

    const LoadDiagnostic& GetLoadDiagnostic() const { return m_loadDiagnostic; }

private:
    void RenderElements(cairo_t* ctx) const;

    std::string m_tempAssetDir;
    std::vector<uint8_t> m_thumbnail;
    LoadDiagnostic m_loadDiagnostic;
};

namespace ogt {
// Canonical single-element (de)serializers for the .ogt element schema.
// Consumers (e.g. the editor's clipboard/undo snapshots) should use these
// rather than duplicating the logic. SerializeElement here keeps raw asset
// paths (no @-bundling); archive bundling stays internal to Title::Save.
std::unique_ptr<VisualElement> ParseElement(const nlohmann::json& j);
nlohmann::json SerializeElement(const IElement* el, const IElement* root);
}
