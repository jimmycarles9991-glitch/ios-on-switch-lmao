/* SPDX-License-Identifier: MIT */
#ifndef NVK_SWITCH_PUSH_TEST_H
#define NVK_SWITCH_PUSH_TEST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define NO_VALUE UINT32_MAX

/* The value the push's last write of `method` carries, NO_VALUE where it has
 * none: Fermi's method headers (nv_push.h), every opcode NVK emits
 */
static inline uint32_t
last_write(const uint32_t *dw, uint32_t count, uint32_t method)
{
   uint32_t value = NO_VALUE;
   for (uint32_t i = 0; i < count;) {
      const uint32_t hdr = dw[i++];
      const uint32_t opcode = hdr >> 29, at = (hdr & 0x1fff) << 2;
      const uint32_t n = (hdr >> 16) & 0x1fff;
      if (opcode == 4) {
         if (at == method)
            value = n;
         continue;
      }
      for (uint32_t k = 0; k < n && i < count; k++, i++) {
         uint32_t m = at;
         if (opcode == 1)
            m = at + 4 * k;
         else if (opcode == 5)
            m = k ? at + 4 : at;
         if (m == method)
            value = dw[i];
      }
   }
   return value;
}

static int failures;

static inline void
check(bool ok, const char *what)
{
   if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      failures++;
   }
}

#endif
