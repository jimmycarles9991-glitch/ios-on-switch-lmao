// SPDX-License-Identifier: BSD-2-Clause-Patent
// The header's inline keys as calls, for the Rust side's test (key_test.rs)
#include "kits/SwitchRefusal/SwitchRefusal.h"

uint64_t switch_refusal_test_key(const char *domain, const char *reason_key)
{
    return switch_refusal_key(domain, reason_key);
}

uint64_t switch_refusal_test_key_with(uint64_t key, uint64_t detail)
{
    return switch_refusal_key_with(key, detail);
}
