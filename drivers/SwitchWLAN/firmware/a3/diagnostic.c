/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "diagnostic.h"

#if DEVELOPMENT || DEBUG
struct switch_a3_diagnostic switch_a3_diagnostic = {
    .magic = SWITCH_A3_DIAGNOSTIC_MAGIC,
    .version = SWITCH_A3_DIAGNOSTIC_VERSION,
    .bytes = sizeof(struct switch_a3_diagnostic),
};

__attribute__((noinline, noclone)) void switch_a3_diagnostic_count(uint32_t word_index)
{
    ++((uint32_t *)&switch_a3_diagnostic)[word_index];
}
__attribute__((noinline, noclone)) void switch_a3_diagnostic_pm(uint32_t value)
{
    switch_a3_diagnostic.pm_reason = (uint8_t)(value >> 8);
    switch_a3_diagnostic.pm_status = (uint8_t)value;
}
#endif
