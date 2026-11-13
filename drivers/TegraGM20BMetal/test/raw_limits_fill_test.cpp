// SPDX-License-Identifier: MIT
// raw_limits_fill.h over a model of the raw device's classes: _MTLDevice's accessors read the limits table, the m2v
// device's class answers linearTextureAlignmentBytes with a method of its own (16), and the raw class answers 32.
#include "../raw/runtime/raw_limits_fill.h"

#include <gtest/gtest.h>
#include <objc/message.h>

#include <string>
#include <vector>

namespace {
uint8_t gTable[kMTLDeviceLimitsBytes];

const LimitField &field(const char *selector)
{
    for (const LimitField &f : kLimitFields)
        if (!strcmp(f.selector, selector))
            return f;
    std::abort();
}

unsigned long tableAccessor(id, SEL sel)
{
    uint32_t v = 0;
    memcpy(&v, gTable + field(sel_getName(sel)).offset, 4);
    return v;
}

unsigned long m2vAnswer(id, SEL)
{
    return 16;
}

unsigned long rawAnswer(id, SEL)
{
    return 32;
}

Class makeClass(Class super, const char *name, IMP imp)
{
    Class cls = objc_allocateClassPair(super, name, 0);
    class_addMethod(cls, sel_registerName("linearTextureAlignmentBytes"), imp, "Q16@0:8");
    objc_registerClassPair(cls);
    return cls;
}
}  // namespace

TEST(RawLimitsFill, AnAlignmentTheM2vClassAnswersItselfReachesTheTable)
{
    Class owner = makeClass(objc_getClass("NSObject"), "LimitsOwnerModel", (IMP)tableAccessor);
    Class m2v = makeClass(owner, "LimitsM2vModel", (IMP)m2vAnswer);
    Class rawDevice = makeClass(m2v, "LimitsRawModel", (IMP)rawAnswer);
    id device = class_createInstance(rawDevice, 0);
    const LimitField &alignment = field("linearTextureAlignmentBytes");
    const uint32_t m2vValue = 16;
    memcpy(gTable + alignment.offset, &m2vValue, 4);
    std::vector<std::pair<std::string, LimitFill>> reports;
    fillRawLimits(device, owner, gTable, [&](const LimitField &f, LimitFill how) {
        reports.emplace_back(f.selector, how);
    });
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].first, "linearTextureAlignmentBytes");
    EXPECT_EQ(reports[0].second, LimitFill::Filled);
    EXPECT_EQ(tableAccessor(device, sel_registerName("linearTextureAlignmentBytes")), 32u);
}
