// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "visual_element.h"
#include <memory>
#include <string>

class ImageElement : public VisualElement {
public:
    std::string imagePath;
    ScaleMode imageScaleMode{ScaleMode::Stretch};
    double imageTileScale{1.0};

    void SetContent(const std::string& value) override { imagePath = value; }

protected:
    void RenderContent(cairo_t* ctx) const override;

private:
    mutable std::shared_ptr<cairo_surface_t> m_image{};
    mutable std::string m_imageCachePath{};
};
