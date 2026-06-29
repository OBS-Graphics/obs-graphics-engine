// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include "element.h"

class Spatial : public IElement {
public:
    Point GetGlobalPosition() const override;

    const Rectangle& GetBounds() const { return m_bounds; }
    void SetBounds(const Rectangle& bounds) { m_bounds = bounds; }

    Point GetPosition() const { return {m_bounds.x, m_bounds.y}; }
    void SetPosition(const Point& pos) { m_bounds.x = pos.x; m_bounds.y = pos.y; }

    Size GetSize() const { return {m_bounds.width, m_bounds.height}; }
    void SetSize(const Size& size) { m_bounds.width = size.width; m_bounds.height = size.height; }

    float GetRotation() const { return m_rotation; }
    void SetRotation(float rotation) { m_rotation = rotation; }

    float GetShearX() const { return m_shearX; }
    void SetShearX(float shearX) { m_shearX = shearX; }

    float GetShearY() const { return m_shearY; }
    void SetShearY(float shearY) { m_shearY = shearY; }

    Transform GetTransform() const { return {m_rotation, m_shearX, m_shearY}; }

    // Adjusts local position to maintain world position when reparented.
    void SetParent(IElement* parent) override;

protected:
    Rectangle m_bounds{0.0, 0.0, 100.0, 100.0};
    float m_rotation{0.0f};
    float m_shearX{0.0f};
    float m_shearY{0.0f};
};
