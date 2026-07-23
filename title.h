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

    IDataSource* dataSource{nullptr};
    size_t dataRecordIndex{0};

    // Fired whenever TriggerIn/TriggerOut runs, regardless of origin (host call,
    // duration timeout, or a script's trigger_in()/trigger_out()). Multiple
    // subscribers (e.g. a host UI and a ScriptDataSource) can listen at once.
    std::vector<std::function<void(size_t recordIndex, double duration)>> onTriggerIn;
    std::vector<std::function<void()>> onTriggerOut;

    Title();
    ~Title();
    Title(const Title&) = delete;
    Title& operator=(const Title&) = delete;
    Title(Title&&) = default;
    Title& operator=(Title&&) = default;

    IElement* GetRoot() const { return elements.empty() ? nullptr : elements[0].get(); }

    void TriggerIn(size_t recordIndex = 0, double duration = -1.0);
    void TriggerOut();
    void UpdateData();
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

    double updateTimer{0.0}, prevUpdateTimer{0.0};
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
