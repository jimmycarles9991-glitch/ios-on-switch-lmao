// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <functional>
#include <vector>

namespace raw {

struct RetirementWrites {
    struct Target {
        void *buffer = nullptr;
        void *texture = nullptr;
    };

    std::vector<void *> buffers, textures;

    template <class Visit, class Resolve> static RetirementWrites capture(Visit visit, Resolve resolve)
    {
        RetirementWrites result;
        result.captureInto(visit, resolve);
        return result;
    }

    // The same into these lists, which keep their capacity
    template <class Visit, class Resolve> void captureInto(Visit visit, Resolve resolve)
    {
        buffers.clear();
        textures.clear();
        visit([&](void *object) {
            const Target target = resolve(object);
            if (target.buffer)
                buffers.push_back(target.buffer);
            if (target.texture)
                textures.push_back(target.texture);
        });
        for (auto *objects : {&buffers, &textures}) {
            std::sort(objects->begin(), objects->end(), std::less<void *>{});
            objects->erase(std::unique(objects->begin(), objects->end()), objects->end());
        }
    }
};

}  // namespace raw
