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
    if (size_ < sizeof(PackHeader) || memcmp(hdr_->magic, "GM20BPK1", 8)) {
        err = "not a raw pack";
        return false;
    }
    if (hdr_->version != kPackVersion) {
        err = "a version-" + std::to_string(hdr_->version) + " pack: this runtime reads version " +
              std::to_string(kPackVersion) + " only";
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
    case RelocTicketsLo:
        return uint32_t(a.tickets);
    case RelocTicketsHi:
        return uint32_t(a.tickets >> 32);
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
    const PackPipeline *fallback = nullptr;
    for (; lo < n && keyAt(lo) == keyHash; lo++) {
        const PackPipeline *at = p + (index_.empty() ? lo : index_[lo]);
        if (at->flags & PipelineVariantRecord)
            continue;
        if (form == PackForm::FetchFree || (at->flags & PipelineFetchFreeForm)) {
            if (form == PackForm::FetchFree && (at->flags & PipelineFetchFreeForm))
                return at;
        } else if (form == PackForm::Barrier) {
            if (at->flags & PipelineBarrierForm)
                return at;
        } else {
            if (at->flags & PipelineStorageMetadata)
                return at;
            if (!fallback)
                fallback = at;
        }
    }
    return fallback;
}

std::vector<const PackPipeline *> Pack::variantRecords(uint64_t keyHash) const
{
    const PackPipeline *p = section<PackPipeline>(PackPipelines);
    const uint32_t n = hdr_->pipelines;
    const auto at = [&](uint32_t i) {
        return p + (index_.empty() ? i : index_[i]);
    };
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (at(mid)->keyHash < keyHash)
            lo = mid + 1;
        else
            hi = mid;
    }
    std::vector<const PackPipeline *> out;
    for (; lo < n && at(lo)->keyHash == keyHash; lo++)
        if (at(lo)->flags & PipelineVariantRecord)
            out.push_back(at(lo));
    return out;
}

const PackPipeline *Pack::variantRecord(uint64_t keyHash, const std::string &name, PackForm form) const
{
    for (const PackPipeline *rec : variantRecords(keyHash))
        if (name == string(rec->variantName) && bool(rec->flags & PipelineBarrierForm) == (form == PackForm::Barrier))
            return rec;
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

const PackSplit *Pack::split(uint32_t wholeProgram) const
{
    const PackSplit *s = section<PackSplit>(PackSplits);
    const PackSplit *end = s + bytes(PackSplits) / sizeof(PackSplit);
    const PackSplit *at = std::lower_bound(s, end, wholeProgram, [](const PackSplit &x, uint32_t w) {
        return x.whole < w;
    });
    return at != end && at->whole == wholeProgram ? at : nullptr;
}

PackForm recordForm(const PackPipeline &pp)
{
    return pp.flags & PipelineFetchFreeForm ? PackForm::FetchFree
           : pp.flags & PipelineBarrierForm ? PackForm::Barrier
                                            : PackForm::Default;
}

const PackIteratedBlend *Pack::iterated(const PackPipeline &pp) const
{
    if (!pp.iterated || uint64_t(pp.iterated) * sizeof(PackIteratedBlend) > bytes(PackIterated))
        return nullptr;
    return section<PackIteratedBlend>(PackIterated) + pp.iterated - 1;
}

namespace {
const uint32_t *spillWords(const Pack &pack, uint32_t at, uint32_t words)
{
    if (!words || (uint64_t(at) + words) * sizeof(uint32_t) > pack.bytes(PackBlob))
        return nullptr;
    return pack.section<uint32_t>(PackBlob) + at;
}
}  // namespace

const uint32_t *Pack::spills(const PackPipeline &pp) const
{
    return spillWords(*this, pp.spillAt, uint32_t(pp.spillWords[0]) + pp.spillWords[1]);
}

const uint32_t *Pack::spills(const PackKernel &k) const
{
    return spillWords(*this, k.spillAt, k.spillWords);
}

const PackVariant *Pack::variants(const PackPipeline &pp, uint32_t stage, uint32_t &count) const
{
    count = pp.variantCount[stage];
    const uint64_t first = uint64_t(pp.variants) + (stage ? pp.variantCount[0] : 0);
    if (!count || (first + count) * sizeof(PackVariant) > bytes(PackVariants)) {
        count = 0;
        return nullptr;
    }
    return section<PackVariant>(PackVariants) + first;
}

const PackArchive *Pack::archive(uint64_t pathHash) const
{
    if (bytes(PackArchives) < 8)
        return nullptr;
    const uint32_t *counts = section<uint32_t>(PackArchives);
    if (8 + uint64_t(counts[0]) * sizeof(PackArchive) + uint64_t(counts[1]) * 8 > bytes(PackArchives))
        return nullptr;
    const PackArchive *a = reinterpret_cast<const PackArchive *>(counts + 2), *end = a + counts[0];
    const PackArchive *at = std::lower_bound(a, end, pathHash, [](const PackArchive &x, uint64_t h) {
        return x.pathHash < h;
    });
    return at != end && at->pathHash == pathHash && uint64_t(at->first) + at->count <= counts[1] ? at : nullptr;
}

const PackComputeFacts *Pack::computeFacts(uint64_t keyHash) const
{
    const PackComputeFacts *f = section<PackComputeFacts>(PackComputeStates);
    const PackComputeFacts *end = f + bytes(PackComputeStates) / sizeof *f;
    const PackComputeFacts *at = std::lower_bound(f, end, keyHash, [](const PackComputeFacts &x, uint64_t h) {
        return x.keyHash < h;
    });
    return at != end && at->keyHash == keyHash ? at : nullptr;
}

bool Pack::declares(const PackArchive &archive, uint64_t keyHash) const
{
    const uint32_t *counts = section<uint32_t>(PackArchives);
    const uint64_t *keys =
        reinterpret_cast<const uint64_t *>(reinterpret_cast<const PackArchive *>(counts + 2) + counts[0]) +
        archive.first;
    return std::binary_search(keys, keys + archive.count, keyHash);
}

}  // namespace raw
