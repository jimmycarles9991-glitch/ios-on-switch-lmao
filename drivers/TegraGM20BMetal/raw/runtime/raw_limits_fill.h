// SPDX-License-Identifier: MIT
// The raw device's own answers in the limits table it inherits (LimitsTable.h). Its class subclasses the m2v device's,
// which answers the alignment selectors (and others) with methods of its own above _MTLDevice: fillLimitsFromMethods
// reads each write back through the immediate superclass, the m2v class, so it never sees the table and restores the
// field ("not read there"). Each answer the raw class overrides is read back here through `owner`, the class whose
// accessors read the table.
#pragma once
#include <objc/runtime.h>
#include <cstring>
#include "LimitsTable.h"

template <class Report> void fillRawLimits(id device, Class owner, uint8_t *table, Report report)
{
    using Getter = unsigned long (*)(id, SEL);
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
        value = reinterpret_cast<Getter>(method_getImplementation(own))(device, sel);
        return true;
    };
    auto readBack = [&](const LimitField &field) -> uint64_t {
        SEL sel = sel_registerName(field.selector);
        Method reader = class_getInstanceMethod(owner, sel);
        return reader ? reinterpret_cast<Getter>(method_getImplementation(reader))(device, sel) : ~0ull;
    };
    fillLimitsTable(table, answer, readBack, report);
}
