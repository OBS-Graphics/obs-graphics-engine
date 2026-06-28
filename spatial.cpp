// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "spatial.h"

Point Spatial::GetGlobalPosition() const
{
    Point pos{m_bounds.x, m_bounds.y};
    if (m_parent) {
        Point parentPos = m_parent->GetGlobalPosition();
        pos.x += parentPos.x;
        pos.y += parentPos.y;
    }
    return pos;
}

void Spatial::SetParent(IElement* parent)
{
    if (m_parent == parent) return;

    // Save world position before the parent changes so we can convert to the new local space.
    Point worldPos = GetGlobalPosition();

    IElement::SetParent(parent); // manages m_parent and the children lists

    if (m_parent) {
        Point parentWorld = m_parent->GetGlobalPosition();
        m_bounds.x = worldPos.x - parentWorld.x;
        m_bounds.y = worldPos.y - parentWorld.y;
    } else {
        m_bounds.x = worldPos.x;
        m_bounds.y = worldPos.y;
    }
}
