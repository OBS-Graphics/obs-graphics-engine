// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "visual_element.h"
#include "qr.hpp"
#include <string>

class QrElement : public VisualElement {
public:
    std::string text;

    void ApplyContent(const std::string& value) override { text = value; }

protected:
    void RenderContent(cairo_t* ctx) const override;

private:
    mutable std::string m_oldText{"\xff"};
    mutable qr::DynQr m_qr{};
};
