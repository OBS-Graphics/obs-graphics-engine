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
    // User-defined, not `= default`. A defaulted move leaves the moved-from
    // m_tempAssetDir in the standard's "valid but unspecified" state, and in
    // practice (libstdc++'s small-string optimization, for a path short
    // enough to fit inline) that state is a COPY of the string, not an empty
    // one. ~Title deletes whatever m_tempAssetDir names, so a moved-from
    // Title going out of scope would then delete the directory the moved-to
    // Title just inherited. Title::Load returns by value into
    // std::make_unique<Title>(Title::Load(...)), so this move happens on
    // every load — see title.cpp for the definitions.
    Title(Title&& other) noexcept;
    Title& operator=(Title&& other) noexcept;

    IElement* GetRoot() const { return elements.empty() ? nullptr : elements[0].get(); }

    // Identity as a data source (and a Lua script) sees it.
    TitleRef Ref() const { return TitleRef{id, name}; }

    // Shows the title. Before animating in, fetches fresh records from the
    // pool via DataPool::DataBlocking (bounded, but it CAN block — showing
    // stale or blank data on first appearance would be worse) and applies
    // them instantly. No-op fetch when there's no pool or no dataSourceId.
    void TriggerIn(size_t recordIndex = 0, double duration = -1.0);

    // Same, but applies records the caller already fetched instead of
    // fetching. This is the form for a host whose Scene is serialized by a
    // lock its render thread also needs: pull with DataPool::DataBlocking off
    // that thread (DataBlocking takes no Scene lock — see data-pool.h), then
    // call this under the lock. The overload above would re-fetch and hold the
    // lock for the whole fetch, which for a network-backed ScriptDataSource is
    // seconds of frozen rendering.
    //
    // Empty `records` means "the source genuinely returned nothing", not "go
    // and fetch" — hence a separate overload rather than a defaulted argument.
    void TriggerIn(size_t recordIndex, double duration, const std::vector<Record>& records);

    void TriggerOut();

    // Points this title at a different source (empty = unbound). Use this
    // rather than assigning `dataSourceId` directly: UpdateData() only
    // applies records when the pool's cache version differs from
    // m_lastDataVersion, and that counter carries over from whatever source
    // was read before. A freshly registered source is primed to version 1,
    // and a static file source sits at version 1 for the whole session — so
    // a bare reassignment can leave a Visible title comparing 1 against 1,
    // reading "unchanged", and rendering the previous source's record
    // indefinitely. Resetting the counter to 0 (a version no live cache ever
    // has) makes the next Visible tick pull the new source unconditionally.
    void SetDataSource(const std::string& id);

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

    // Everything both TriggerIn overloads do once the data has been applied:
    // the state transition and the notifications. Kept in one place so the two
    // entry points can never drift on callback or relay ordering.
    void EnterAnimatingIn(size_t recordIndex, double duration);

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
