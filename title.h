// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "data-source.h"
#include "element.h"
#include "uuid.h"
#include "visual_element.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class DataPool;

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

    // Stable machine identity, unique within a Scene (Scene::AddTitle
    // regenerates it on collision — e.g. the same .ogt opened twice). This is
    // what a Lua script targets with trigger_out(title) and what
    // Scene::FindById looks up. Round-trips through title.json.
    std::string id{uuid::GenerateV4()};
    // Human-readable name — what an operator sees and what a script searches
    // with scene.find_titles(name). Not unique: two titles may share a name,
    // which is why that lookup returns a list. This is the field pre-1.1
    // .ogt files stored (misleadingly) as "id".
    std::string name;
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

    // Id of the IDataSource this title reads, as registered with `dataPool`
    // (IDataSource::GetId). Empty means "no data source" — the title renders
    // whatever content its elements already hold. Deliberately NOT persisted
    // in title.json: which source a title reads is the host's mapping to
    // own, per collection/profile.
    std::string dataSourceId;
    // The pool to read `dataSourceId` from. Set by Scene::AddTitle; a title
    // with no pool simply never pulls data.
    DataPool* dataPool{nullptr};

    // Which record of the source's record list this title shows; set by
    // TriggerIn and reused by every subsequent poll while Visible.
    size_t dataRecordIndex{0};

    // Fired whenever TriggerIn/TriggerOut runs, regardless of origin (host call,
    // duration timeout, or a script's trigger_out()). Multiple subscribers
    // (e.g. a host UI and an editor preview) can listen at once.
    std::vector<std::function<void(size_t recordIndex, double duration)>> onTriggerIn;
    std::vector<std::function<void()>> onTriggerOut;

    Title();
    ~Title();
    Title(const Title&) = delete;
    Title& operator=(const Title&) = delete;
    Title(Title&&) = default;
    Title& operator=(Title&&) = default;

    IElement* GetRoot() const { return elements.empty() ? nullptr : elements[0].get(); }

    // Identity as a data source (and a Lua script) sees it.
    TitleRef Ref() const { return TitleRef{id, name}; }

    // Shows the title. Before animating in, fetches fresh records from the
    // pool via DataPool::DataBlocking (bounded, but it CAN block — showing
    // stale or blank data on first appearance would be worse) and applies
    // them instantly. No-op fetch when there's no pool or no dataSourceId.
    void TriggerIn(size_t recordIndex = 0, double duration = -1.0);
    void TriggerOut();

    // Pulls the pool's cache for `dataSourceId` and applies it with the
    // animated SetContent, but only when that cache actually changed since
    // the last pull (DataPool versions each cache, so this is an integer
    // compare on an unchanged source). Called every tick while Visible.
    void UpdateData();

    // Applies `records[dataRecordIndex % records.size()]` to matching
    // elements by id (SetContentInstant if `instant`, else the animated
    // SetContent). No-op if `records` is empty. The primitive both TriggerIn
    // and UpdateData() funnel through; also usable directly by a host that
    // has records of its own and no data source at all.
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

    // Version of the pool cache this title last applied; see
    // DataPool::DataIfChanged. 0 = nothing applied yet.
    uint64_t m_lastDataVersion{0};
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
