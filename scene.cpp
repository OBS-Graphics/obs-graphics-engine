// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "scene.h"

#include "uuid.h"

#include <algorithm>
#include <iostream>
#include <unordered_set>

Title* Scene::AddTitle(std::unique_ptr<Title> title)
{
    if (!title) return nullptr;

    if (title->id.empty())
        title->id = uuid::GenerateV4();

    // Ids round-trip through .ogt, so opening the same file twice hands us
    // two Titles claiming the same identity. Whoever arrives second gets a
    // fresh one — otherwise FindById and a script's trigger_out(title) would
    // both be ambiguous.
    while (FindById(title->id) != nullptr) {
        std::string taken = title->id;
        title->id = uuid::GenerateV4();
        std::cerr << "Scene::AddTitle: duplicate title id '" << taken << "' (name '" << title->name
                  << "'), reassigned to '" << title->id << "'" << std::endl;
    }

    title->dataPool = &m_pool;

    m_titles.push_back(std::move(title));
    return m_titles.back().get();
}

void Scene::RemoveTitle(Title* title)
{
    auto it = std::find_if(m_titles.begin(), m_titles.end(),
                           [&](const std::unique_ptr<Title>& t) { return t.get() == title; });
    if (it == m_titles.end()) return;
    m_titles.erase(it);
}

void Scene::Clear()
{
    m_titles.clear();
}

std::vector<Title*> Scene::FindByName(const std::string& name) const
{
    std::vector<Title*> found;
    for (const auto& t : m_titles)
        if (t->name == name) found.push_back(t.get());
    return found;
}

Title* Scene::FindById(const std::string& id) const
{
    for (const auto& t : m_titles)
        if (t->id == id) return t.get();
    return nullptr;
}

void Scene::Tick(float dt)
{
    // (a) sources: pump + refresh caches on their own cadence.
    m_pool.Tick(dt);

    // (b) title directory: only republish when it actually changed, so the
    // common frame does one vector compare and nothing else.
    std::vector<TitleRef> directory;
    directory.reserve(m_titles.size());
    for (const auto& t : m_titles)
        directory.push_back(t->Ref());

    if (directory != m_publishedDirectory) {
        m_pool.PublishTitleDirectory(directory);
        m_publishedDirectory = std::move(directory);
    }

    // (c) script-originated trigger_out(...) requests.
    for (auto& [sourceId, requested] : m_pool.DrainOutTriggerRequests()) {
        std::unordered_set<Title*> firedThisDrain;

        auto fire = [&](Title* title) {
            if (!firedThisDrain.insert(title).second) return;
            // Re-applying TriggerOut() to a title already off (or on its way
            // off) screen would reset its timer, replay the out-animation and
            // re-relay _on_trigger_out to the script.
            if (title->state == TitleState::Hidden || title->state == TitleState::AnimatingOut)
                return;
            title->TriggerOut();
        };

        for (const std::string& id : requested) {
            if (id.empty()) {
                // Lua's bare trigger_out(): every title reading this source.
                for (const auto& t : m_titles)
                    if (t->dataSourceId == sourceId) fire(t.get());
                continue;
            }
            // A named request can target ANY title in the scene, not just one
            // reading this source — a script sees the whole scene through
            // scene.find_titles(), so e.g. a breaking-news script can hide an
            // unrelated ticker. Only the bare-sentinel form above is scoped to
            // the source's own titles.
            if (Title* t = FindById(id))
                fire(t);
        }
    }

    // (d) titles: each pulls its own data from the pool inside Tick.
    for (auto& t : m_titles)
        t->Tick(dt);
}

std::vector<Title*> Scene::RenderOrder() const
{
    std::vector<Title*> ordered;
    ordered.reserve(m_titles.size());
    for (const auto& t : m_titles)
        if (t->state != TitleState::Hidden) ordered.push_back(t.get());

    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Title* a, const Title* b) { return a->zOrder < b->zOrder; });
    return ordered;
}

void Scene::Render(cairo_t* ctx) const
{
    for (Title* t : RenderOrder())
        t->Render(ctx);
}

void Scene::Render(cairo_t* ctx, int outputWidth, int outputHeight) const
{
    for (Title* t : RenderOrder())
        t->Render(ctx, outputWidth, outputHeight);
}
