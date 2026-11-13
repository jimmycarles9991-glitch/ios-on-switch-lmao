// SPDX-License-Identifier: MIT
// What a Core Animation state's name (its label after "com.apple.coreanimation.", caspec27.py's grammar) says about the
// draws the runtime's shortcuts may take: a fill (no texture function `_T` and no image `_I`: the colour is the vertex
// stream's) or a blur-chain pass (texture functions ds4/6/8, db2/4, nb7, n11-n27, sb3: weight[i] * sample(uv + offset[i])
// over the fragment uniform at buffer 1, raw_blur_twod.h).
#pragma once
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace raw {

enum class CaRoute : uint8_t {
    None,
    Fill,
    Blur
};

inline CaRoute caRouteOf(const char *name)
{
    if (!name || !*name)
        return CaRoute::None;
    for (const char *f : {"_Tds", "_Tdb", "_Tnb", "_Tn1", "_Tn2", "_Tsb"})
        if (strstr(name, f))
            return CaRoute::Blur;
    return strstr(name, "_T") || strstr(name, "_I") ? CaRoute::None : CaRoute::Fill;
}

}  // namespace raw
