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
 * last owner left, and their lines may still be in the CPU's cache.
 */
IOMemoryDescriptor *switch_iokit_fresh_memory(uint64_t bytes, uint64_t run, void **cpu);
