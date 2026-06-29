// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "data-source.h"
#include "element.h"
#include "visual_element.h"
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

enum class TitleState { Hidden, AnimatingIn, Visible, AnimatingOut };

struct Title {
    std::string id;
    int width{1920}, height{1080};
    nlohmann::json metadata;  // arbitrary editor/consumer data, preserved round-trip
    std::vector<std::unique_ptr<IElement>> elements;  // [0] is always the auto-created root
    TitleState state{TitleState::Hidden};
    double timer{0.0};
    int zOrder{0};

    IDataSource* dataSource{nullptr};
    size_t dataRecordIndex{0};

    Title();
    ~Title();
    Title(const Title&) = delete;
    Title& operator=(const Title&) = delete;
    Title(Title&&) = default;
    Title& operator=(Title&&) = default;

    IElement* GetRoot() const { return elements.empty() ? nullptr : elements[0].get(); }

    void TriggerIn(size_t recordIndex = 0);
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

private:
    void RenderElements(cairo_t* ctx) const;

    double updateTimer{0.0}, prevUpdateTimer{0.0};
    std::string m_tempAssetDir;
    std::vector<uint8_t> m_thumbnail;
};
