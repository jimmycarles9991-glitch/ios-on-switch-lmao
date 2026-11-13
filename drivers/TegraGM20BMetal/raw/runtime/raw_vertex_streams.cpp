// SPDX-License-Identifier: MIT
// A draw's fixed-function vertex streams (raw_gpu.h RenderEncoder): stream 0 at the record's vertexSlot, the streams past it
// at its vertexStreams, each bound as NVK_MME_BIND_VB binds one.
#include <algorithm>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "raw_gpu.h"

namespace raw {
namespace {

enum : uint32_t {
    kStreamLocation = NVB197_SET_VERTEX_STREAM_A_LOCATION_A(0),
    kStreamFormat = NVB197_SET_VERTEX_STREAM_A_FORMAT(0),
    kStreamEnable = NVDEF(NVB197, SET_VERTEX_STREAM_A_FORMAT, ENABLE, TRUE),
    kStreamLimit = NVB197_SET_VERTEX_STREAM_LIMIT_A_A(0),
    kStreamMethods = NVB197_SET_VERTEX_STREAM_A_FORMAT(1) - NVB197_SET_VERTEX_STREAM_A_FORMAT(0),
    kLimitMethods = NVB197_SET_VERTEX_STREAM_LIMIT_A_A(1) - NVB197_SET_VERTEX_STREAM_LIMIT_A_A(0),
};

constexpr uint64_t kBytesChunk = 0x100;  // a setBytes binding's upload block (m2v's)

}  // namespace

void RenderEncoder::bindStreams(Push &p, const Pipeline &pl, bool indexed, uint32_t vFirst, uint32_t count)
{
    const PackPipeline &r = *pl.rec;
    if (r.vertexSlot == 0xff)
        return;
    const uint32_t stride0 = r.vertexStrideDynamic ? vertexStride(r.vertexSlot) : r.vertexStride;
    bindStream(p, 0, r.vertexSlot, stride0, r.vertexStrideDynamic, indexed, vFirst, count);
    for (uint32_t k = 1; k < kPackVertexStreams && r.vertexStreams[k - 1]; k++) {
        const uint8_t stream = r.vertexStreams[k - 1], slot = streamSlot(stream);
        const bool bindStride = stream & kStreamBindStride;
        bindStream(p, k, slot, bindStride ? vertexStride(slot) : pl.streamStride[k], bindStride, indexed, vFirst,
                   count);
    }
}

void RenderEncoder::bindStream(Push &p, uint32_t stream, uint8_t slot, uint32_t stride, bool bindStride, bool indexed,
                               uint32_t vFirst, uint32_t count)
{
    const Bound &b = bufs_[1][slot];
    // the stream ends where the bound bytes end: a setBytes chunk or the buffer's end
    uint64_t location = b.va, bytes = b.buffer ? b.size : (b.size + kBytesChunk - 1) & ~(kBytesChunk - 1);
    if (!location || !bytes) {  // none bound, or none of it left: NVK's empty stream (nvk_mme_bind_vb before Turing)
        location = dev_->zero_.va;
        bytes = 0;
    }
    const uint64_t limit = location + bytes - 1;
    if (location != streamVa_[stream] || limit != streamLimit_[stream]) {
        if (b.buffer)
            cb_->use(*b.buffer, false);
        else if (!b.named)
            cb_->useAddress(b.va, b.size, false);
        p.inc(kStreamLocation + kStreamMethods * stream, {uint32_t(location >> 32), uint32_t(location)});
        p.inc(kStreamLimit + kLimitMethods * stream, {uint32_t(limit >> 32), uint32_t(limit)});
        streamVa_[stream] = location;
        streamLimit_[stream] = limit;
    }
    if (bindStride)
        p.m(kStreamFormat + kStreamMethods * stream, kStreamEnable | stride);
    if (b.buffer && b.buffer->hostVisible && !b.buffer->wc && stride) {
        const uint64_t from = std::min(b.offset + (indexed ? 0 : uint64_t(vFirst) * stride), b.buffer->length);
        const uint64_t cleaned =
            std::min<uint64_t>(indexed ? b.size : uint64_t(count) * stride, b.buffer->length - from);
        cb_->clean(b.buffer->cpu + from, cleaned);
    }
}

}  // namespace raw
