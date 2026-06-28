// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#pragma once

#include <string>
#include <vector>

#include "animation.h"
#include "types.hpp"

class IElement {
public:
    virtual ~IElement() = default;

    const std::string& GetId() const { return m_id; }
    void SetId(const std::string& id) { m_id = id; }

    IElement* GetParent() const { return m_parent; }
    virtual void SetParent(IElement* parent);

    const std::vector<IElement*>& GetChildren() const { return m_children; }

    void AddChild(IElement* element);
    void RemoveChild(IElement* element);

    virtual Point GetGlobalPosition() const { return {0.0, 0.0}; }

    virtual void Render(cairo_t* ctx, const AnimatedTransform& xf,
                        const AnimatedTransform* maskXf,
                        double timer, bool isOut,
                        double parentOffX = 0.0, double parentOffY = 0.0) const {}

    virtual void ApplyClipping(cairo_t* ctx, const AnimatedTransform& xf,
                               const AnimatedTransform* maskXf,
                               double parentOffX, double parentOffY) const {}

protected:
    void AddChildDirect(IElement* child);
    void RemoveChildDirect(IElement* child);

    std::string m_id;
    IElement* m_parent{nullptr};
    std::vector<IElement*> m_children;
};
