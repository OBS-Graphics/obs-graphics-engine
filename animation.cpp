// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>

#include "animation.h"

#include "element.h"

namespace animation {
AnimatedTransform EvaluateAnimation(const AnimationDef& def, double timer, bool isOut,
                                    const Element& el)
{
    AnimatedTransform out{};

    double t = (timer - def.delay) / def.duration;
    double p = animation::ApplyEasing(std::clamp(t, 0.0, 1.0), def.easing);
    double rp = 1.0 - p;

    auto fnFade = [&]() {
        out.opacity = isOut ? rp : p;
    };

    switch (def.type) {
    case AnimationType::None:
        break;
    case AnimationType::Fade: fnFade(); break;
    case AnimationType::SlideUp:
        out.offsetY = (isOut ? -p : rp) * el.bounds.height;
        fnFade();
        break;
    case AnimationType::SlideDown:
        out.offsetY = (isOut ? p : -rp) * el.bounds.height;
        fnFade();
        break;
    case AnimationType::SlideLeft:
        out.offsetX = (isOut ? p : -rp) * el.bounds.width;
        fnFade();
        break;
    case AnimationType::SlideRight:
        out.offsetX = (isOut ? -p : rp) * el.bounds.width;
        fnFade();
        break;
    case AnimationType::WipeUp:
        out.clipY = isOut ? 0.0 : rp;
        out.clipH = isOut ? rp : p;
        break;
    case AnimationType::WipeDown:
        out.clipY = isOut ? p : 0.0;
        out.clipH = isOut ? rp : p;
        break;
    case AnimationType::WipeLeft:
        out.clipX = isOut ? 0.0 : rp;
        out.clipW = isOut ? rp : p;
        break;
    case AnimationType::WipeRight:
        out.clipX = isOut ? p : 0.0;
        out.clipW = isOut ? rp : p;
        break;
    case AnimationType::ScaleIn:
        out.scale = isOut ? rp : p;
        fnFade();
        break;
    }

    return out;
}
} // namespace animation
