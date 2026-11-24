// SPDX-License-Identifier: MIT
// See pack.h.
#include "pack.h"
#include "set_entries.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace gm20b {

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
        if (hdr_->section[s].offset > size_ || hdr_->section[s].bytes > size_ - hdr_->section[s].offset ||
            hdr_->tier0[s] > hdr_->section[s].bytes) {
            err = "truncated pack";
            return false;
        }
    if (uint64_t(hdr_->pipelines) * sizeof(PackPipeline) > bytes(PackPipelines) ||
        uint64_t(hdr_->programs) * sizeof(PackProgram) > bytes(PackPrograms)) {
        err = "a pipeline or program count past its section";
        return false;
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

namespace {

// The entries of [first, end) whose key is `key`, each tier's part of the span ([first, split), [split, end)) in key order
template <class T, class Key>
std::pair<const T *, const T *> tieredRange(const T *first, const T *split, const T *end, uint64_t key, Key keyOf)
{
    for (const auto &[from, to] : {std::pair{first, split}, std::pair{split, end}}) {
        const T *at = std::lower_bound(from, to, key, [&](const T &entry, uint64_t wanted) {
            return keyOf(entry) < wanted;
        });
        const T *last = at;
        while (last != to && keyOf(*last) == key)
            last++;
        if (last != at)
            return {at, last};
    }
    return {end, end};
}

}  // namespace

PackRun Pack::run(uint64_t keyHash) const
{
    const PackPipeline *p = section<PackPipeline>(PackPipelines);
    const auto [first, last] = tieredRange(p, p + tier0Entries(PackPipelines, sizeof(PackPipeline)),
                                           p + hdr_->pipelines, keyHash, [](const PackPipeline &r) {
                                               return r.keyHash;
                                           });
    return first == last ? PackRun{} : PackRun{first, uint32_t(last - first)};
}

PackRun Pack::fastRun(uint64_t fast) const
{
    const PackFastKey *k = section<PackFastKey>(PackFastKeys);
    const auto [at, last] = tieredRange(k, k + tier0Entries(PackFastKeys, sizeof(PackFastKey)),
                                        k + bytes(PackFastKeys) / sizeof(PackFastKey), fast, [](const PackFastKey &f) {
                                            return f.fast;
                                        });
    if (at == last)
        return {};
    return {section<PackPipeline>(PackPipelines) + at->first, at->records};
}

const PackPipeline *Pack::find(const PackRun &run, PackForm form) const
{
    const PackPipeline *fallback = nullptr;
    for (const PackPipeline *at = run.first; at != run.first + run.records; at++) {
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

const PackPipeline *Pack::find(uint64_t keyHash, PackForm form) const
{
    return find(run(keyHash), form);
}

std::vector<const PackPipeline *> Pack::variantRecords(const PackRun &run) const
{
    std::vector<const PackPipeline *> out;
    for (const PackPipeline *at = run.first; at != run.first + run.records; at++)
        if (at->flags & PipelineVariantRecord)
            out.push_back(at);
    return out;
}

std::vector<const PackPipeline *> Pack::variantRecords(uint64_t keyHash) const
{
    return variantRecords(run(keyHash));
}

const PackPipeline *Pack::variantRecord(const PackRun &run, const std::string &name, PackForm form) const
{
    for (const PackPipeline *rec : variantRecords(run))
        if (name == string(rec->variantName) && bool(rec->flags & PipelineBarrierForm) == (form == PackForm::Barrier))
            return rec;
    return nullptr;
}

const PackPipeline *Pack::variantRecord(uint64_t keyHash, const std::string &name, PackForm form) const
{
    return variantRecord(run(keyHash), name, form);
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

const uint32_t *Pack::far(const PackPipeline &pp) const
{
    return entryCount(pp) > kPackEntries ? section<uint32_t>(PackBlob) + farEntriesAt(pp) : nullptr;
}

const uint16_t *Pack::farUbos(const PackProgram &program) const
{
    const uint32_t at = programFarAt(program);
    return at ? reinterpret_cast<const uint16_t *>(section<uint32_t>(PackBlob) + at - 1) : nullptr;
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

const uint8_t *Pack::reflection(uint64_t keyHash, bool compute, uint32_t &length) const
{
    const uint64_t size = bytes(PackReflections);
    const uint8_t *at = section<uint8_t>(PackReflections);
    uint32_t counts[2] = {};
    if (size < sizeof counts)
        return nullptr;
    memcpy(counts, at, sizeof counts);
    if ((uint64_t(counts[0]) + counts[1]) * sizeof(PackReflection) > size - sizeof counts)
        return nullptr;
    const auto *first = reinterpret_cast<const PackReflection *>(at + sizeof counts) + (compute ? counts[0] : 0);
    const PackReflection *end = first + counts[compute ? 1 : 0];
    const PackReflection *found = std::lower_bound(first, end, keyHash, [](const PackReflection &r, uint64_t h) {
        return r.keyHash < h;
    });
    if (found == end || found->keyHash != keyHash || found->offset > size || found->bytes > size - found->offset)
        return nullptr;
    length = found->bytes;
    return at + found->offset;
}

bool Pack::declares(const PackArchive &archive, uint64_t keyHash) const
{
    const uint32_t *counts = section<uint32_t>(PackArchives);
    const uint64_t *keys =
        reinterpret_cast<const uint64_t *>(reinterpret_cast<const PackArchive *>(counts + 2) + counts[0]) +
        archive.first;
    return std::binary_search(keys, keys + archive.count, keyHash);
}

}  // namespace gm20b
