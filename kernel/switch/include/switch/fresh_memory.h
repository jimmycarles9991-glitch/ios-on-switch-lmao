// SPDX-License-Identifier: GPL-2.0-only
/*
 * Wired kernel memory with its pages' earlier contents left in place, for a buffer a device fills before anything reads
 * it: the GPU driver's spares, which its copy engine zero-fills (drivers/TegraGM20B/TegraGM20BUserClientSpares.cpp).
 * IOKit's own allocators always pass KMA_ZERO, which zero-fills every page through the CPU.
 */
#pragma once
#include <stdint.h>

class IOMemoryDescriptor;

/*
 * `bytes`, in whole VM pages, as one descriptor (not prepared) of wired kernel pages, freed with the descriptor once
 * nothing holds it, and its kernel address in `cpu`; nullptr when the VM has no memory. With `run` set, the pages are one
 * physically contiguous run aligned to `run` bytes (a power of two above the page size). The pages hold whatever their
 * last owner left, and their lines may still be in the CPU's cache, unless write-combined: then the pages' VM object
 * is write-combined before this returns, as an IOBufferMemoryDescriptor's with kIOMapWriteCombineCache, so every
 * mapping of them (`cpu` and the physical aperture included) is write-combined and their lines were cleaned and
 * invalidated.
 */
enum switch_iokit_fresh_cache {
    SWITCH_IOKIT_FRESH_CACHED,
    SWITCH_IOKIT_FRESH_WRITE_COMBINED
};

IOMemoryDescriptor *switch_iokit_fresh_memory(uint64_t bytes, uint64_t run, switch_iokit_fresh_cache cache, void **cpu);
