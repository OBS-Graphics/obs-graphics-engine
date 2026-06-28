// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "visual_element.h"

class RectangleElement : public VisualElement {
protected:
    void RenderContent(cairo_t* ctx) const override;
};
