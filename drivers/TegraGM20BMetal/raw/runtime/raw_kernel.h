// SPDX-License-Identifier: MIT
// The TegraGM20B user client (drivers/TegraGM20B/TegraGM20BABI.h) as the raw GM20B path calls it: the same service, open type,
// call forms (direct, or tunnelled through the one selector a sandbox admits) and records as Mesa NVK's nvkmd_darwin.
// The connection is NVK's (switch_nvk_connection), so the GPU VM is the one m2v draws in: a BO and VA of either driver are the
// other's too, and m2v's views of the raw path's memory need no import or bind of their own. VAs come from the quarter NVK leaves
// out, the top one (a GPU fault address says which path made it), through a first-fit free list (VA is given back when a BO is
// freed). A process without NVK (a host tool of the raw path alone) gets a connection of its own, the same quarter.
// Not thread safe: raw::Device calls it under its own lock.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include "TegraGM20BABI.h"
#include "raw_surfaces.h"

namespace raw {

struct Bo {
    uint64_t handle = 0;  // kernel BO
    uint64_t va = 0;      // where it is bound in the connection's GPU VM (an import: the first byte asked for)
    uint64_t bytes = 0;   // what is bound (an import: whole pages)
    uint8_t *cpu =
        nullptr;  // CPU address of va (allocations: IOConnectMapMemory64 of map_token; imports: the caller's pages)
    uint32_t token = 0;   // allocations: the map token (unmapped at free)
    uint64_t vaBase = 0;  // the bound range's start (an import's va is inside its first page)
    uint64_t mapped = 0;  // CPU mapping bytes (allocations)
};

class Kernel {
public:
    ~Kernel();
    bool open(std::string &err);

    // The connection is NVK's: m2v's views of this path's memory may name its BOs and VAs.
    bool shared() const
    {
        return shared_;
    }

    const TegraGM20BAbiInfo &info() const
    {
        return info_;
    }

    bool alloc(uint64_t bytes, bool coherent, Bo &bo, uint32_t kind = 0, uint64_t align = 0x10000);
    // Wires [addr, addr + bytes) of this task (whole pages) and binds them; bo.va/bo.cpu name addr itself.
    bool import(void *addr, uint64_t bytes, Bo &bo, uint32_t kind = 0, uint32_t surfaceId = 0);
    // The same in two steps, for a caller that serialises the VA list itself: the kernel's BoImport (the wiring: the slow part,
    // safe beside the other calls) and then the VA and its bind (bindImport, under the caller's lock).
    bool importPages(void *addr, uint64_t bytes, Bo &bo, uint32_t surfaceId = 0);
    // The GPU may write the IOSurface from now on (BoImport took it as only sampled): before its first render or store.
    bool surfaceGpuWrites(uint32_t surfaceId);
    // SurfaceLayout (method 16) for the import `bo` of a surface: its layout, decided by the kernel once for every process
    // at the first asking import (TegraGM20BLayout.h: block-linear when that importer writes it alone). `writes`: this
    // texture renders into or stores to it. TegraGM20BLayoutTiled, TegraGM20BLayoutOtherShape, or 0 (pitch; also without
    // the method).
    uint32_t surfaceLayout(const Bo &bo, uint32_t width, uint32_t height, uint32_t bpp, uint32_t bytesPerRow,
                           bool writes);
    bool bindImport(void *addr, Bo &bo, uint32_t kind);
    // BoFree (the kernel unbinds it at once and releases the pages after the channels complete what they had); the VA goes
    // back to the free list at once: call it only once no submission that uses the BO is still running (raw::Device does).
    void free(Bo &bo);
    bool channel(uint32_t engines, uint32_t &channel, const volatile uint32_t *&fence);
    void closeChannel(uint32_t channel);

    // An Exec or WaitFence found `channel` dead (the kernel killed it for a GPU fault or hang; its fence was released, and
    // the connection's other channels, BOs and binds go on)
    bool lost(uint32_t channel) const
    {
        return lostChannel_.load(std::memory_order_relaxed) == channel;
    }

    // `last`: the channel's last submitted seqno (0 if none), waited on when the GPFIFO is full
    // `surfaces` (null: none): the host imports the submission touches, named to the kernel (ExecSurfaces, method 17,
    // TegraGM20BInfoExecSurfaces) before each Exec of it: it waits on the GPU for other channels' writes to them, and counts
    // as a writer of the ones it writes only. A kernel without it, or a list it refuses, leaves the Exec as one with none.
    bool exec(uint32_t channel, const TegraGM20BAbiPush *pushes, uint32_t count, uint32_t last, uint32_t &seqno,
              SurfaceList *surfaces = nullptr);
    // Blocks in the kernel until the channel's completed seqno passes `seqno` (the kext sleeps on the fence interrupt).
    bool wait(uint32_t channel, uint32_t seqno, uint64_t timeoutNs = ~0ull);
    // Order (method 14, TegraGM20BInfoOrder): GPU-side acquires between `channel` and the task's other channels (NVK's).
    // The kern_return_t; `waits`: the acquires queued.
    int order(uint32_t channel, uint32_t flags, uint32_t seqno, uint32_t &waits);

    bool canOrder() const
    {
        return (info_.flags & TegraGM20BInfoOrder) != 0;
    }

    const std::string &error() const
    {
        return err_;
    }

    uint64_t vaInUse() const
    {
        return vaUsed_;
    }

private:
    int call(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize);
    bool map(uint32_t token, bool readOnly, uint64_t &address, uint64_t &bytes);
    void unmap(uint32_t token, uint64_t address);
    bool bind(uint64_t va, uint64_t handle, uint64_t range, uint32_t kind);
    uint64_t allocVa(uint64_t bytes, uint64_t align);
    void freeVa(uint64_t va, uint64_t bytes);
    bool openOwn(std::string &err);
    uint32_t conn_ = 0;
    bool tunnel_ = false;
    bool shared_ = false;
    void (*release_)() = nullptr;  // NVK's switch_nvk_connection_release
    // A SurfaceWrites failed: imports stop saying the GPU only samples them, and every Exec carries
    // TegraGM20BPushImportsWritten.
    std::atomic<bool> importsWritten_{false};
    std::atomic<uint32_t> lostChannel_{0};
    uint64_t fenceMap_ = 0;  // the connection's fence page, mapped once: every channel's word is in it
    uint32_t selector_ = 0;
    TegraGM20BAbiInfo info_ = {};
    std::map<uint64_t, uint64_t> vaFree_;  // start -> bytes, coalesced
    uint64_t vaUsed_ = 0;
    std::string err_;
};

}  // namespace raw
