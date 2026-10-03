#pragma once

#include <QColor>

#include "constants/design_tokens.h"
#include "model/machine.h"

namespace app {

// Accent palette behind ElementColor, shared by item painting and the action box's
// swatch menu. Blue is not the #3B82F6 selection blue, so a blue element still
// visibly changes when selected.
// ElementColor::Default returns an invalid QColor; callers use their standard look.
inline QColor elementAccent(ElementColor color) {
    switch (color) {
        case ElementColor::Red:
            return design::color(design::kElementRed);
        case ElementColor::Orange:
            return design::color(design::kElementOrange);
        case ElementColor::Yellow:
            return design::color(design::kElementYellow);
        case ElementColor::Green:
            return design::color(design::kElementGreen);
        case ElementColor::Blue:
            return design::color(design::kElementBlue);
        case ElementColor::Purple:
            return design::color(design::kElementPurple);
        case ElementColor::Pink:
            return design::color(design::kElementPink);
        case ElementColor::Gray:
            return design::color(design::kElementGray);
        case ElementColor::Default:
        default:
            return QColor();
    }
}

}  // namespace app
