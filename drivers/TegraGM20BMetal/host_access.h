// SPDX-License-Identifier: MIT
#pragma once
#include "kernel.h"
#include <utility>
#include <vector>

namespace gm20b {
void reportCpuReleaseFailure();

inline bool hostRangeContains(const void *base, uint64_t extent, const void *cpu, uint64_t bytes)
{
    const uintptr_t start = uintptr_t(base), address = uintptr_t(cpu);
    return base && cpu && bytes && address >= start && address - start <= extent && bytes <= extent - (address - start);
}

class HostAccess {
public:
    struct Range {
        Bo bo;
        uint64_t offset, bytes;
    };

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

    bool acquire(Kernel &kernel, std::vector<Range> ranges, bool write, void *owner, uint32_t arena,
                 void (*release)(void *, uint32_t))
    {
        if (kernel_ || ranges.empty()) {
            release(owner, arena);
            return false;
        }
        ranges_ = std::move(ranges);
        owner_ = owner;
        arena_ = arena;
        release_ = release;
        access_ = write ? TegraGM20BCpuWrite : TegraGM20BCpuRead;
        kernel_ = &kernel;
        for (const Range &range : ranges_) {
            if (!kernel.cpuAccess(range.bo, range.offset, range.bytes, access_, true)) {
                finish();
                return false;
            }
            ++acquired_;
        }
        return true;
    }

    bool finish()
    {
        if (!kernel_)
            return true;
        if (!ranges_.empty()) {
            // The arena lease goes whatever the kernel answers: nothing would release it later
            bool released = true;
            for (; acquired_; --acquired_) {
                const Range &range = ranges_[acquired_ - 1];
                released = kernel_->cpuAccess(range.bo, range.offset, range.bytes, access_, false) && released;
            }
            kernel_ = nullptr;
            ranges_.clear();
            release_(owner_, arena_);
            release_ = nullptr;
            owner_ = nullptr;
            return released;
        }
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
    std::vector<Range> ranges_;
    size_t acquired_ = 0;
    void *owner_ = nullptr;
    uint32_t arena_ = 0;
    void (*release_)(void *, uint32_t) = nullptr;
};
}  // namespace gm20b
