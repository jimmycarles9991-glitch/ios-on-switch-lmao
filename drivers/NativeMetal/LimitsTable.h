// SPDX-License-Identifier: MIT
// The base class's limits table (-[_MTLDevice limits], the words after isa). Metal's accessors read their field from it
// (-[_MTLDevice maxTextureWidth2D] is limits+0x84), and frameworks read it directly, past the device's methods:
// PhotosUICore's texture converter takes its largest image from 0x84/0x88 (-[PXGMetalTextureConverter
// createTextureFromCGImage:transform:alpha:options:], PXGCreateMetalTextureFromBytesInCGImage); with them 0 every image is
// "too large", is redrawn to fit 0x0 and PXGCreateContext asserts. CoreRE reads minBufferNoCopyAlignmentBytes at 0x7c. The
// fields and their offsets are the selected IPSW kind's (kLimitFields, MetalABI.h: drivers/NativeMetal/abi).
#pragma once
#include <cstdint>
#include <cstring>

#include "MetalABI.h"

enum class LimitFill : uint8_t {
    Filled,
    Overflow,  // the answer does not fit the field's 32 bits
    NotRead,   // the base class's accessor does not read the field there: it is left as it was
};

// Writes each field the device answers into `table`: answer(field, value) is false where it has no answer of its own (the field
// stays the base class's). Every write is read back through readBack(field), the base class's own accessor; a field it does not
// read there (21H461 answers minConstantBufferAlignmentBytes from elsewhere) is restored, and the fill goes on with the next
// field, each checked alike. report(field, how) hears every field answered.
template <class Answer, class ReadBack, class Report>
void fillLimitsTable(uint8_t *table, Answer answer, ReadBack readBack, Report report)
{
    for (const LimitField &field : kLimitFields) {
        uint64_t value = 0;
        if (!answer(field, value))
            continue;
        if (field.bytes == 4 && value > UINT32_MAX) {
            report(field, LimitFill::Overflow);
            continue;
        }
        uint8_t was[8];
        memcpy(was, table + field.offset, field.bytes);
        if (field.bytes == 8) {
            memcpy(table + field.offset, &value, 8);
        } else {
            const uint32_t word = uint32_t(value);
            memcpy(table + field.offset, &word, 4);
        }
        if (readBack(field) != value) {
            memcpy(table + field.offset, was, field.bytes);
            report(field, LimitFill::NotRead);
            continue;
        }
        report(field, LimitFill::Filled);
    }
}

#ifdef __OBJC__
#include <objc/runtime.h>

// fillLimitsTable with the answers of `device`'s own class: each field's method where it is not the base class's and answers an
// integer, read back through the base class's.
template <class Report> void fillLimitsFromMethods(id device, uint8_t *table, Report report)
{
    Class cls = object_getClass(device), base = class_getSuperclass(cls);
    auto answer = [&](const LimitField &field, uint64_t &value) {
        SEL sel = sel_registerName(field.selector);
        Method own = class_getInstanceMethod(cls, sel), original = class_getInstanceMethod(base, sel);
        if (!own || !original || method_getImplementation(own) == method_getImplementation(original))
            return false;
        char type[8] = {};
        method_getReturnType(own, type, sizeof type);
        if (strcmp(type, "Q") && strcmp(type, "L") && strcmp(type, "I"))
            return false;
        value = ((NSUInteger (*)(id, SEL))method_getImplementation(own))(device, sel);
        return true;
    };
    auto readBack = [&](const LimitField &field) -> uint64_t {
        SEL sel = sel_registerName(field.selector);
        return ((NSUInteger (*)(id, SEL))method_getImplementation(class_getInstanceMethod(base, sel)))(device, sel);
    };
    fillLimitsTable(table, answer, readBack, report);
}
#endif
