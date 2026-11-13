// SPDX-License-Identifier: MIT
// A queue's or a command buffer's residency sets (addResidencySet:, useResidencySet:). The raw path needs none: every allocation
// of either driver is mapped in the one GPU VM they share (raw_kernel.h) and stays resident, so a raw command buffer answers them
// itself and stays raw. Only m2v work of that command buffer uses them: the queue's and the command buffer's sets on each m2v
// command buffer the replay makes, so the queue's m2v twin is never made or told for them.
#pragma once
#include "raw_lock.h"
#import <Metal/Metal.h>
#import <objc/message.h>
#include <algorithm>
#include <mutex>
#include <vector>

namespace raw {

class ResidencySets {
public:
    void add(const id __unsafe_unretained *sets, NSUInteger count)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        for (NSUInteger i = 0; i < count; i++)
            if (sets[i] && std::find(sets_.begin(), sets_.end(), sets[i]) == sets_.end())
                sets_.push_back(sets[i]);
    }

    void remove(const id __unsafe_unretained *sets, NSUInteger count)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        for (NSUInteger i = 0; i < count; i++)
            sets_.erase(std::remove(sets_.begin(), sets_.end(), sets[i]), sets_.end());
    }

    void useOn(id<MTLCommandBuffer> m2v) const
    {
        std::vector<id> now;
        {
            std::lock_guard<raw::Mutex> l(lock_);
            if (sets_.empty())
                return;
            now = sets_;
        }
        // by name: residency sets are iOS 18 API and this also builds for iOS 17
        static const SEL use = NSSelectorFromString(@"useResidencySets:count:");
        if (![(id)m2v respondsToSelector:use])
            return;
        std::vector<__unsafe_unretained id> list(now.begin(), now.end());
        ((void (*)(id, SEL, const __unsafe_unretained id *, NSUInteger))objc_msgSend)(m2v, use, list.data(),
                                                                                      list.size());
    }

private:
    mutable raw::Mutex lock_;
    std::vector<id> sets_;
};

}  // namespace raw
