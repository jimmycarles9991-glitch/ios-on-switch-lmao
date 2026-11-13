// SPDX-License-Identifier: MIT
// How the GPU maps each use of the raw path's control memory (Kernel::allocControl: the kernel's control reserve, mapped
// write-combined for the CPU). A job invalidates the GPU's L2 only when it says the CPU wrote what it reads, and an
// internal job's writes may still sit in the L2 after its fence:
//  - Volatile (TegraGM20BBindMapVolatile): the L2 keeps none of its lines, so what the CPU writes for a job, and what the
//    CPU reads after one, needs no L2 flush or invalidate. Every GPU access is a DRAM round trip.
//  - Cached: memory the GPU alone reads and writes after the CPU's one fill, made between its bind and the first
//    submission, which invalidates the L2 for the new mapping (TegraGM20BTlb.h); or memory the CPU writes now and then,
//    named host-written to the next submission's job so the kernel invalidates the L2 first. No CPU reads it.
#pragma once
#include <cstdint>

namespace raw {

enum class ControlMapping : uint8_t {
    Volatile,
    Cached
};

enum class ControlUse : uint8_t {
    TextureHeaders,    // the CPU writes a header per new texture; the header cache absorbs the GPU's reads
    SamplerHeaders,    // likewise, per new sampler
    Stamps,            // the GPU writes a time per command buffer, which the CPU reads (Device::gpuNanoseconds)
    Ring,              // push and upload rings: the CPU writes them per command buffer
    Bytes,             // setBytes chunks: the CPU writes them per draw
    TransferSlot,      // a copy's staging: the CPU writes it before the copy engine's job, or reads it after
    RootTable,         // LOAD_CONSTANT_BUFFER writes it per draw, the constant cache reads it
    ZeroPage,          // texture header 0 and unbound buffers read it, per pixel
    InterlockTickets,  // NAK's pixel interlock spins on and atomically increments its words, per 32-pixel group
    ColorTable,        // sRGB colour writes read it, per pixel
    StateSegments,  // the CPU appends a state's methods at its creation, after the GPU fetched the chunk's earlier ones
    // m2v's handle tables and heap sets (raw_heap_tables.h): the CPU writes an entry per texture or sampler an argument
    // buffer may name and names them host-written to the next job (Device::tablesWritten_); programs search them per pixel
    HandleTables,
    // m2v's root table, its identity resident set, sink and ExecutionStatus words: one CPU fill, then the GPU's writes
    ArgumentRoots,
};

constexpr ControlMapping controlMapping(ControlUse use)
{
    switch (use) {
    case ControlUse::RootTable:
    case ControlUse::ZeroPage:
    case ControlUse::InterlockTickets:
    case ControlUse::ColorTable:
    case ControlUse::ArgumentRoots:
    case ControlUse::HandleTables:
        return ControlMapping::Cached;
    case ControlUse::TextureHeaders:
    case ControlUse::SamplerHeaders:
    case ControlUse::Stamps:
    case ControlUse::Ring:
    case ControlUse::Bytes:
    case ControlUse::TransferSlot:
    case ControlUse::StateSegments:
        return ControlMapping::Volatile;
    }
    return ControlMapping::Volatile;
}

}  // namespace raw
