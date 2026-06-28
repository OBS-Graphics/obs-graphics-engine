// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "element.h"

#include <algorithm>

void IElement::SetParent(IElement* parent)
{
    if (m_parent == parent) return;
    if (m_parent) m_parent->RemoveChildDirect(this);
    m_parent = parent;
    if (m_parent) m_parent->AddChildDirect(this);
}

void IElement::AddChild(IElement* element)
{
    if (element) element->SetParent(this);
}

void IElement::RemoveChild(IElement* element)
{
    if (element && element->GetParent() == this)
        element->SetParent(nullptr);
}

void IElement::AddChildDirect(IElement* child)
{
    auto it = std::find(m_children.begin(), m_children.end(), child);
    if (it == m_children.end())
        m_children.push_back(child);
}

void IElement::RemoveChildDirect(IElement* child)
{
    auto it = std::find(m_children.begin(), m_children.end(), child);
    if (it != m_children.end())
        m_children.erase(it);
}
