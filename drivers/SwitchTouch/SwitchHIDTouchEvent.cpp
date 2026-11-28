// SPDX-License-Identifier: BSD-2-Clause-Patent
// Builds digitizer collection IOHIDEvents directly for SwitchTouch, bypassing IOHIDDevice::handleReportWithTime's
// 60-element HID report parsing and second work-loop gate. Matches IOHIDEventDriver::handleDigitizerCollectionReport
// and IOHIDEvent::digitizerEvent on both 21H461 (iOS 17) and 24B119 (iOS 27).
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSObject.h>
#include "SwitchHIDTouchEvent.h"

namespace {
// IOHIDEvent virtual slots and object field offsets verified in both 21H461 and 24B119 IOHIDFamily kernelcaches:
// _data at +0x10 (_data->options at +0x08), _options at +0x48.
enum : unsigned {
    kSlotAppendChild = 15,
    kSlotSetTimeStamp = 17,
    kSlotGetType = 18,
    kSlotSetType = 19,
    kSlotGetInteger = 23,
    kSlotSetInteger = 24,
    kSlotGetFixed = 25,
    kSlotSetFixed = 26,
};

constexpr size_t kEventDataOffset = 0x10;
constexpr size_t kEventOptionsOffset = 0x48;
constexpr size_t kDataOptionsOffset = 0x08;

// IOHIDDigitizerEventData::orientationType offset differs by build because 24B119 widened position/radius fields
// from 32-bit IOFixed (sizeof 0x68) to 64-bit double (sizeof 0xa0). setFixedValue requires orientationType == Quality
// before storing MajorRadius/MinorRadius.
#if SWITCH_IPSW_KIND_21H461
constexpr size_t kOrientationTypeOffset = 0x40;
#elif SWITCH_IPSW_KIND_24B119
constexpr size_t kOrientationTypeOffset = 0x58;
#else
#error "SwitchHIDTouchEvent.cpp: no IOHIDDigitizerEventData orientationType offset for this IPSW kind"
#endif

enum : uint32_t {
    kEventTypeDigitizer = 11,
    kOptionIsAbsolute = 0x1,
    kTransducerFinger = 2,
    kIdentityFinger = 2,
    kOrientationQuality = 2,
    kFieldX = 0xb0000,
    kFieldY = 0xb0001,
    kFieldType = 0xb0004,
    kFieldIndex = 0xb0005,
    kFieldIdentity = 0xb0006,
    kFieldEventMask = 0xb0007,
    kFieldRange = 0xb0008,
    kFieldTouch = 0xb0009,
    kFieldMajorRadius = 0xb0014,
    kFieldMinorRadius = 0xb0015,
    kFieldCollection = 0xb0016,
};

constexpr int32_t kFingerRadiusFixed = 5 << 16;

template <typename R, typename... A> R vcall(void *object, unsigned slot, A... args)
{
    return reinterpret_cast<R (*)(void *, A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object, args...);
}

OSObject *allocDigitizerEvent(uint64_t timestamp, uint32_t index, int32_t fx, int32_t fy, bool down, uint32_t mask,
                              bool collection, SwitchHIDTouchState &state)
{
    OSObject *event = OSMetaClass::allocClassWithName("IOHIDEvent");
    if (!event) {
        if (!state.verified) {
            IOLog("SwitchHID: direct touch events off: IOHIDEvent class unavailable\n");
            state.broken = true;
        }
        return nullptr;
    }

    vcall<void, uint32_t>(event, kSlotSetType, kEventTypeDigitizer);
    auto *base = reinterpret_cast<uint8_t *>(event);
    auto *data = *reinterpret_cast<uint8_t **>(base + kEventDataOffset);
    if (!data) {
        event->release();
        return nullptr;
    }

    vcall<void, uint64_t>(event, kSlotSetTimeStamp, timestamp);
    *reinterpret_cast<uint32_t *>(base + kEventOptionsOffset) |= kOptionIsAbsolute;
    *reinterpret_cast<uint32_t *>(data + kDataOptionsOffset) |= kOptionIsAbsolute;
    *reinterpret_cast<uint32_t *>(data + kOrientationTypeOffset) = kOrientationQuality;

    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSlotSetInteger, kFieldType, kTransducerFinger, 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSlotSetInteger, kFieldIndex, static_cast<int32_t>(index), 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSlotSetInteger, kFieldIdentity, kIdentityFinger, 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSlotSetFixed, kFieldMajorRadius, kFingerRadiusFixed, 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSlotSetFixed, kFieldMinorRadius, kFingerRadiusFixed, 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSlotSetFixed, kFieldX, fx, 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSlotSetFixed, kFieldY, fy, 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSlotSetInteger, kFieldRange, down ? 1 : 0, 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSlotSetInteger, kFieldTouch, down ? 1 : 0, 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSlotSetInteger, kFieldEventMask, static_cast<int32_t>(mask),
                                                 0);
    if (collection)
        vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSlotSetInteger, kFieldCollection, 1, 0);
    return event;
}

bool verifyEvent(OSObject *event, uint32_t index, int32_t fx, uint32_t mask, bool collection)
{
    const uint32_t type = vcall<uint32_t>(event, kSlotGetType);
    const int32_t gotIndex = vcall<int32_t, uint32_t, IOOptionBits>(event, kSlotGetInteger, kFieldIndex, 0);
    const int32_t gotIdentity = vcall<int32_t, uint32_t, IOOptionBits>(event, kSlotGetInteger, kFieldIdentity, 0);
    const int32_t gotMask = vcall<int32_t, uint32_t, IOOptionBits>(event, kSlotGetInteger, kFieldEventMask, 0);
    const int32_t gotCol = vcall<int32_t, uint32_t, IOOptionBits>(event, kSlotGetInteger, kFieldCollection, 0);
    const int32_t gotX = vcall<int32_t, uint32_t, IOOptionBits>(event, kSlotGetFixed, kFieldX, 0);
    const int32_t gotRadius = vcall<int32_t, uint32_t, IOOptionBits>(event, kSlotGetFixed, kFieldMajorRadius, 0);
    return type == kEventTypeDigitizer && gotIndex == static_cast<int32_t>(index) && gotIdentity == kIdentityFinger &&
           gotMask == static_cast<int32_t>(mask) && gotCol == (collection ? 1 : 0) && gotX == fx &&
           gotRadius == kFingerRadiusFixed;
}
}  // namespace

OSObject *switch_hid_touch_build_event(const SwitchTouchReport &report, uint32_t edgeContacts, uint64_t timestamp,
                                       SwitchHIDTouchState &state)
{
    if (state.broken)
        return nullptr;

    SwitchHIDTouchPlan plan = {};
    if (!switch_hid_touch_plan(report, edgeContacts, state, plan))
        return nullptr;

    OSObject *collection = allocDigitizerEvent(timestamp, 0, plan.centroidX, plan.centroidY, plan.anyDown,
                                               plan.collectionMask, true, state);
    if (!collection)
        return nullptr;

    for (unsigned i = 0; i < plan.count; ++i) {
        const SwitchHIDTouchFinger &f = plan.fingers[i];
        OSObject *child = allocDigitizerEvent(timestamp, f.index, f.fx, f.fy, f.down, f.eventMask, false, state);
        if (!child) {
            collection->release();
            return nullptr;
        }
        if (!state.verified && i == 0 && !verifyEvent(child, f.index, f.fx, f.eventMask, false)) {
            IOLog("SwitchHID: direct touch events off: finger IOHIDEvent readback mismatch\n");
            state.broken = true;
            child->release();
            collection->release();
            return nullptr;
        }
        vcall<void, OSObject *>(collection, kSlotAppendChild, child);
        child->release();
    }

    if (!state.verified) {
        if (!verifyEvent(collection, 0, plan.centroidX, plan.collectionMask, true)) {
            IOLog("SwitchHID: direct touch events off: collection IOHIDEvent readback mismatch\n");
            state.broken = true;
            collection->release();
            return nullptr;
        }
        state.verified = true;
    }

    return collection;
}
