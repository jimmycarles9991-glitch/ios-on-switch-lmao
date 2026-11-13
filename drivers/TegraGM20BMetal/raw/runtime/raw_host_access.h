// SPDX-License-Identifier: MIT
#pragma once
#include "raw_kernel.h"

namespace raw {
void reportCpuReleaseFailure();

inline bool hostRangeContains(const void *base, uint64_t extent, const void *cpu, uint64_t bytes)
{
    const uintptr_t start = uintptr_t(base), address = uintptr_t(cpu);
    return base && cpu && bytes && address >= start && address - start <= extent && bytes <= extent - (address - start);
}

class HostAccess {
public:
    HostAccess() = default;
    HostAccess(const HostAccess &) = delete;
    HostAccess &operator=(const HostAccess &) = delete;

    ~HostAccess()
    {
        if (!finish())
            reportCpuReleaseFailure();
    }

    bool acquire(Kernel &kernel, const Bo &bo, const void *cpu, uint64_t bytes, bool write)
    {
        uint64_t offset = 0;
        if (kernel_ || !bo.handle || !bytes || !boCpuOffset(bo, cpu, bytes, offset))
            return false;
        offset_ = offset;
        bytes_ = bytes;
        access_ = write ? TegraGM20BCpuWrite : TegraGM20BCpuRead;
        if (!kernel.cpuAccess(bo, offset_, bytes_, access_, true))
            return false;
        bo_ = bo;
        kernel_ = &kernel;
        return true;
    }

    bool finish()
    {
        if (!kernel_)
            return true;
        if (!kernel_->cpuAccess(bo_, offset_, bytes_, access_, false))
            return false;
        kernel_ = nullptr;
        return true;
    }

private:
    Kernel *kernel_ = nullptr;
    Bo bo_;
    uint64_t offset_ = 0, bytes_ = 0;
    uint32_t access_ = 0;
};
}  // namespace raw
