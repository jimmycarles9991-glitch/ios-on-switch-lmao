// SPDX-License-Identifier: MIT
// See raw_pack.h.
#include "raw_pack.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace raw {

uint64_t Pack::fnv(const void *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

Pack::~Pack()
{
    if (base_)
        munmap(const_cast<uint8_t *>(base_), size_);
}

bool Pack::load(const char *path, uint64_t stack, std::string &err)
{
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        err = std::string("cannot open ") + path;
        return false;
    }
    struct stat st;
    fstat(fd, &st);
    void *m = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) {
        err = "mmap failed";
        return false;
    }
    base_ = static_cast<const uint8_t *>(m);
    size_ = st.st_size;
    hdr_ = reinterpret_cast<const PackHeader *>(base_);
    if (size_ < sizeof(PackHeader) || memcmp(hdr_->magic, "GM20BPK1", 8) || hdr_->version != kPackVersion) {
        err = "not a version-" + std::to_string(kPackVersion) + " pack";
        return false;
    }
    if (stack && hdr_->stack != stack) {
        err = "the pack is another build's (stack key differs)";
        return false;
    }
    for (int s = 0; s < PackSectionCount; s++)
        if (hdr_->section[s].offset + hdr_->section[s].bytes > size_) {
            err = "truncated pack";
            return false;
        }
    const PackPipeline *p = section<PackPipeline>(PackPipelines);
    const auto byKey = [p](uint32_t a, uint32_t b) {
        return p[a].keyHash < p[b].keyHash;
    };
    for (uint32_t i = 1; i < hdr_->pipelines && index_.empty(); i++)
        if (p[i].keyHash < p[i - 1].keyHash) {
            index_.resize(hdr_->pipelines);
            for (uint32_t k = 0; k < hdr_->pipelines; k++)
                index_[k] = k;
            std::stable_sort(index_.begin(), index_.end(), byKey);
        }
    return true;
}

uint32_t relocated(const PackInitMethod &m, const InitAddresses &a)
{
    switch (m.reloc) {
    case RelocHeapHi:
        return uint32_t(a.heap >> 32);
    case RelocHeapLo:
        return uint32_t(a.heap);
    case RelocZeroHi:
        return uint32_t(a.zero >> 32);
    case RelocZeroLo:
        return uint32_t(a.zero);
    case RelocRootHi:
        return uint32_t(a.root >> 32);
    case RelocRootLo:
        return uint32_t(a.root);
    case RelocTicHi:
        return uint32_t(a.tic >> 32);
    case RelocTicLo:
        return uint32_t(a.tic);
    case RelocTicMax:
        return a.ticMax;
    case RelocTscHi:
        return uint32_t(a.tsc >> 32);
    case RelocTscLo:
        return uint32_t(a.tsc);
    case RelocTscMax:
        return a.tscMax;
    default:
        return m.data;
    }
}

const PackPipeline *Pack::find(uint64_t keyHash, PackForm form) const
{
    const PackPipeline *p = section<PackPipeline>(PackPipelines);
    const uint32_t n = hdr_->pipelines;
    const auto keyAt = [&](uint32_t i) {
        return p[index_.empty() ? i : index_[i]].keyHash;
    };
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (keyAt(mid) < keyHash)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < n && keyAt(lo) == keyHash; lo++) {
        const PackPipeline *at = p + (index_.empty() ? lo : index_[lo]);
        if (form == PackForm::Default || (at->flags & PipelineBarrierForm))
            return at;
    }
    return nullptr;
}

// mkpack.py writes the kernels in keyHash order
const PackKernel *Pack::findKernel(uint64_t keyHash) const
{
    const PackKernel *k = section<PackKernel>(PackKernels);
    const PackKernel *end = k + bytes(PackKernels) / sizeof(PackKernel);
    const PackKernel *at = std::lower_bound(k, end, keyHash, [](const PackKernel &a, uint64_t h) {
        return a.keyHash < h;
    });
    return at != end && at->keyHash == keyHash ? at : nullptr;
}

const PackSplit *Pack::split(uint16_t wholeProgram) const
{
    const PackSplit *s = section<PackSplit>(PackSplits);
    const PackSplit *end = s + bytes(PackSplits) / sizeof(PackSplit);
    const PackSplit *at = std::lower_bound(s, end, wholeProgram, [](const PackSplit &x, uint16_t w) {
        return x.whole < w;
    });
    return at != end && at->whole == wholeProgram ? at : nullptr;
}

}  // namespace raw
