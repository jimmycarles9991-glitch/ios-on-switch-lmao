// SPDX-License-Identifier: MIT
// NVK's shared connection (nvkmd_darwin.c switch_nvk_connection) as the raw path finds it, in an image named
// libvulkan_nouveau.dylib: the connection the kernel model answers (raw_kernel_model.h ignores its number) and a client VA
// window wide enough for the shader heap's 4 GiB program window, the top quarter NVK leaves out.
#include <stdbool.h>
#include <stdint.h>

__attribute__((visibility("default"))) bool switch_nvk_connection(uint32_t *conn, bool *tunnel, uint32_t *selector,
                                                                  uint64_t *va_start, uint64_t *va_end)
{
    *conn = 42;
    *tunnel = false;
    *selector = 0;
    *va_start = 0x3000000000ull;
    *va_end = 0x4000000000ull;
    return true;
}

__attribute__((visibility("default"))) void switch_nvk_connection_release(void)
{}
