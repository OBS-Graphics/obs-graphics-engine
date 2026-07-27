// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "data-pool.h"
#include "data-source.h"
#include "title.h"

#include <memory>
#include <string>
#include <vector>

// A Scene is one self-contained runtime unit: the Titles a host has open,
// plus the DataPool holding the data sources those Titles read. Hosts keep
// one per collection/profile; persistence is the host's job (there is
// deliberately no Save/Load here — the host rebuilds a Scene from its own
// settings by adding sources first, then Titles carrying the matching
// `dataSourceId`).
//
// Scene is the only place that knows both halves of the picture:
//  - DataPool knows sources and caches, never Titles.
//  - Title knows the id of the source it reads, and pulls that cache itself.
//  - Scene publishes the Title directory into the sources (so a Lua script
//    can look a title up by name) and maps script-originated trigger_out(...)
//    requests back onto the Titles they name.
//
// ── Threading ──
// A Scene and its Titles are single-threaded: Tick/Render/AddTitle/
// RemoveTitle and any direct Title call (TriggerIn/TriggerOut/...) must all
// happen on the same thread, normally the host's render thread. The pool
// underneath is separately thread-safe for source registration, so a host UI
// thread may still Add/Remove sources — see data-pool.h.
class Scene {
public:
    Scene() = default;
    ~Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    DataPool& Pool() { return m_pool; }
    const DataPool& Pool() const { return m_pool; }

    // Takes ownership and returns a borrowed pointer valid until that Title
    // is removed. Points the Title at this Scene's pool, and guarantees id
    // uniqueness within the Scene: a Title whose id is already taken (the
    // same .ogt opened twice — ids round-trip through the file) is given a
    // fresh uuid, so a script's trigger_out(title) can never hit two.
    Title* AddTitle(std::unique_ptr<Title> title);

    // Destroys `title`. No-op if it isn't in this Scene.
    void RemoveTitle(Title* title);
    void Clear();

    const std::vector<std::unique_ptr<Title>>& Titles() const { return m_titles; }

    // Every Title with this name, in insertion order. Names are not unique —
    // that's the point of returning a list (scene.find_titles in Lua).
    std::vector<Title*> FindByName(const std::string& name) const;

    // The Title with this uuid, or nullptr.
    Title* FindById(const std::string& id) const;

    // One frame, in order:
    //  (a) DataPool::Tick — pump sources and refresh caches on their cadence;
    //  (b) hand the {id, name} Title directory to the pool, which works out
    //      per source which sources are behind and need it — including one
    //      registered after the directory last changed (see
    //      DataPool::PublishTitleDirectory);
    //  (c) drain script-originated trigger_out(...) requests and apply them:
    //      a request naming a uuid hits that Title, the empty-string sentinel
    //      (Lua's bare trigger_out()) hits every Title reading that source.
    //      Deduplicated within one drain, and skipped for a Title already
    //      Hidden or AnimatingOut, so a doubled request can't replay an
    //      out-animation or double-relay _on_trigger_out;
    //  (d) Title::Tick on every Title — which is where each pulls its own
    //      data from the pool.
    void Tick(float dt);

    // Draws every non-Hidden Title, ordered by Title::zOrder (stable, so
    // equal zOrders keep insertion order).
    void Render(cairo_t* ctx) const;
    void Render(cairo_t* ctx, int outputWidth, int outputHeight) const;

private:
    std::vector<Title*> RenderOrder() const;

    DataPool m_pool;
    std::vector<std::unique_ptr<Title>> m_titles;
    // Scratch buffer Tick rebuilds the directory into each frame; a member
    // purely so the rebuild reuses its capacity instead of allocating. Not
    // state — the pool owns what was actually published to whom.
    std::vector<TitleRef> m_directoryScratch;
};
