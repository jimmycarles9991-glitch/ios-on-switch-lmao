// SPDX-License-Identifier: MIT
// What a probe draw sets back (probe_restores.h) is what the channel's init context writes (context.tsv, generated as
// SwitchGM20BContext.h), register by register.
#include <gtest/gtest.h>

#include "SwitchGM20BContext.h"
#include "../probe_restores.h"

namespace {

TEST(ProbeRestores, EachIsTheInitContextsValue)
{
    for (const gm20b::ProbeRestore &r : gm20b::kProbeRestores) {
        bool found = false;
        uint32_t value = 0;
        for (const switch_gm20b_context_method &m : switch_gm20b_context)
            if (m.subc == 0 && m.mthd == r.method && m.reloc == SWITCH_GM20B_CONTEXT_FIXED) {
                found = true;
                value = m.data;
            }
        ASSERT_TRUE(found) << std::hex << "0x" << r.method << " is not in the init context";
        EXPECT_EQ(value, r.value) << std::hex << "0x" << r.method;
    }
}

}  // namespace
