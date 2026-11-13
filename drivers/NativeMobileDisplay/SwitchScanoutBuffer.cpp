// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchScanoutBuffer.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

namespace {
void release(void *context)
{
    auto *pages = static_cast<switch_dma_pages *>(context);
    switch_dma_pages_free(pages);
    IODelete(pages, switch_dma_pages, 1);
}
}

switch_gpu_backing *switch_display_scanout_buffer(size_t bytes)
{
    if (!bytes || bytes > (20u << 20))
        return nullptr;
    auto *owner = IONewZero(switch_dma_pages, 1);
    if (!owner)
        return nullptr;
    if (!switch_dma_pages_alloc(owner, bytes)) {
        IODelete(owner, switch_dma_pages, 1);
        return nullptr;
    }
    auto *backing = switch_gpu_backing_register(owner->pages, owner->count, 0, owner, release);
    if (!backing)
        release(owner);
    return backing;
}
