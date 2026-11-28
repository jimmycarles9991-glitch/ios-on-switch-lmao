// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "SwitchHID.h"

class OSObject;

struct SwitchHIDTouchSlotState {
    bool down;
    int32_t fx, fy;
};

struct SwitchHIDTouchState {
    SwitchHIDTouchSlotState slots[kSwitchTouchSlots];
    int32_t centroidX, centroidY;
    bool verified;
    bool broken;
};

struct SwitchHIDTouchFinger {
    uint32_t index;
    int32_t fx, fy;
    uint32_t eventMask;
    bool down;
};

struct SwitchHIDTouchPlan {
    SwitchHIDTouchFinger fingers[kSwitchTouchSlots];
    unsigned count;
    int32_t centroidX, centroidY;
    uint32_t collectionMask;
    bool anyDown;
};

enum : uint32_t {
    kSwitchHidDigitizerMaskRange = 0x00000001u,
    kSwitchHidDigitizerMaskTouch = 0x00000002u,
    kSwitchHidDigitizerMaskPosition = 0x00000004u,
    kSwitchHidDigitizerMaskFromEdgeTip = 0x00000800u,
};

// Matches IOHIDElementPrivate::getScaledFixedValue(kIOHIDValueScaleTypeCalibrated) with [0, 1] calibration.
inline int32_t switch_hid_touch_scale(uint16_t value, uint32_t logicalMax)
{
    const uint32_t clamped = value > logicalMax ? logicalMax : value;
    return static_cast<int32_t>((static_cast<int64_t>(clamped) << 16) / logicalMax);
}

// Plans a digitizer collection matching IOHIDEventDriver::handleDigitizerCollectionReport, emitting child events only
// for contacts that are down or lifted since the last dispatched report. Returns false when nothing needs dispatch.
inline bool switch_hid_touch_plan(const SwitchTouchReport &report, uint32_t edgeContacts, SwitchHIDTouchState &state,
                                  SwitchHIDTouchPlan &plan)
{
    plan.count = 0;
    plan.collectionMask = 0;
    plan.anyDown = false;
    int64_t touchX = 0;
    int64_t touchY = 0;
    unsigned touchCount = 0;

    for (unsigned i = 0; i < kSwitchTouchSlots; ++i) {
        const bool down = (report.slot[i].flags & kSwitchTouchTip) != 0;
        SwitchHIDTouchSlotState &prev = state.slots[i];
        if (!down && !prev.down)
            continue;

        const int32_t fx = switch_hid_touch_scale(report.slot[i].x, kSwitchTouchReportWidth - 1);
        const int32_t fy = switch_hid_touch_scale(report.slot[i].y, kSwitchTouchReportHeight - 1);

        uint32_t mask = 0;
        if (down != prev.down)
            mask |= kSwitchHidDigitizerMaskTouch | kSwitchHidDigitizerMaskRange;
        if (down && (fx != prev.fx || fy != prev.fy))
            mask |= kSwitchHidDigitizerMaskPosition;
        if ((edgeContacts & (1u << i)) != 0)
            mask |= kSwitchHidDigitizerMaskFromEdgeTip;

        prev.down = down;
        if (down) {
            prev.fx = fx;
            prev.fy = fy;
            touchX += fx;
            touchY += fy;
            ++touchCount;
            plan.anyDown = true;
        }
        plan.collectionMask |= mask;
        plan.fingers[plan.count++] = {i, fx, fy, mask, down};
    }

    if (plan.count == 0)
        return false;

    if (touchCount > 0) {
        state.centroidX = static_cast<int32_t>(touchX / static_cast<int64_t>(touchCount));
        state.centroidY = static_cast<int32_t>(touchY / static_cast<int64_t>(touchCount));
    }
    plan.centroidX = state.centroidX;
    plan.centroidY = state.centroidY;
    return true;
}

OSObject *switch_hid_touch_build_event(const SwitchTouchReport &report, uint32_t edgeContacts, uint64_t timestamp,
                                       SwitchHIDTouchState &state);
