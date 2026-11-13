// SPDX-License-Identifier: MIT
// A kernel's record cache entry (raw_cache.h encodeKernelEntry): the entry head, then its PackKernel, key string, program, constant
// buffer types and sampler headers, or its refusal.
#include "raw_cache.h"
#include "raw_cache_io.h"

namespace raw {

std::string encodeKernelEntry(const CacheKey &k, const ComputeRecord *record, const std::string &refusal)
{
    Writer w;
    writeEntryHead(w, k, record ? kKindKernel : kKindKernelRefusal);
    if (record) {
        w.value(record->kernel);
        w.text(record->keyString);
        w.list(record->code);
        w.list(record->data);
        w.bytes(record->cbufType, sizeof record->cbufType);
        w.value(record->cbufs);
        w.list(record->samplers);
        w.list(record->spill);
    } else {
        w.text(refusal);
    }
    w.value(Pack::fnv(w.out.data(), w.out.size()));
    return w.out;
}

RecordCache::Found decodeKernelEntry(const std::string &bytes, const CacheKey &k, ComputeRecord &record,
                                     std::string &refusal)
{
    Reader r(bytes);
    uint32_t kind = 0;
    if (!readEntryHead(bytes, r, k, kind))
        return RecordCache::Found::None;
    if (kind == kKindKernelRefusal) {
        refusal = r.text();
        return r.ok() && r.at() == bytes.size() - 8 ? RecordCache::Found::Refusal : RecordCache::Found::None;
    }
    if (kind != kKindKernel)
        return RecordCache::Found::None;
    ComputeRecord rec;
    rec.kernel = r.value<PackKernel>();
    rec.keyString = r.text();
    rec.code = r.list<uint8_t>();
    rec.data = r.list<uint8_t>();
    r.bytes(rec.cbufType, sizeof rec.cbufType);
    rec.cbufs = r.value<uint8_t>();
    rec.samplers = r.list<std::array<uint32_t, 8>>();
    rec.spill = r.list<uint32_t>();
    if (!r.ok() || r.at() != bytes.size() - 8 || rec.cbufs > sizeof rec.cbufType ||
        rec.spill.size() != rec.kernel.spillWords ||
        rec.kernel.entries > sizeof rec.kernel.entry / sizeof rec.kernel.entry[0])
        return RecordCache::Found::None;
    record = std::move(rec);
    return RecordCache::Found::Record;
}

}  // namespace raw
