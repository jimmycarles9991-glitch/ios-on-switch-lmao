// SPDX-License-Identifier: MIT
#import "RawMetalPassCache.h"
#include <objc/runtime.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include "raw_objc_layout.h"

namespace {

constexpr size_t kAttachments = 10;  // 8 colour, depth, stencil
// a compositor frame's passes, each its own shape (boot 21: 29-40 render targets a command buffer), kept to the next
constexpr size_t kEntries = 64;
// A shape no pass took for this many lookups is let go, with the textures its copy holds: a frame's passes take theirs
// within a few
constexpr uint64_t kIdleLookups = 4 * kEntries;

// Where Metal's descriptor classes keep what a pass reads, from their ivars' names and encodings
struct Layout {
    bool ok = false;
    Class descriptor = nil, array = nil, attachment[3] = {}, samples = nil, sample = nil;
    size_t descriptorBytes = 0, attachmentBytes[3] = {};
    ptrdiff_t attachmentsAt = 0, sampleBuffersAt = 0;  // in the descriptor
    ptrdiff_t slotAt[kAttachments] = {};               // each attachment's object in the attachment array
    ptrdiff_t textureAt[3] = {};                       // an attachment's texture in its object
    ptrdiff_t samplesAt = 0, sampleBufferAt = 0;       // the sample buffer attachments in their array, a buffer in one
};

bool privateField(Class cls, const char *field, char kind, ptrdiff_t &at, size_t instance)
{
    Ivar ivar = class_getInstanceVariable(cls, "_private");
    const char *type = ivar ? ivar_getTypeEncoding(ivar) : nullptr;
    std::vector<raw::objc::Field> fields;
    size_t size = 0;
    if (!type || !raw::objc::structFields(type, fields, size) || size_t(ivar_getOffset(ivar)) + size > instance)
        return false;
    const raw::objc::Field *f = raw::objc::field(fields, field, kind);
    if (!f)
        return false;
    at = ivar_getOffset(ivar) + ptrdiff_t(f->offset);
    return true;
}

bool ivarAt(Class cls, const char *name, ptrdiff_t &at)
{
    Ivar ivar = class_getInstanceVariable(cls, name);
    if (!ivar)
        return false;
    at = ivar_getOffset(ivar);
    return true;
}

Layout makeLayout()
{
    Layout l;
    @autoreleasepool {
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        id kinds[3] = {d.colorAttachments[0], d.depthAttachment, d.stencilAttachment};
        id sample = d.sampleBufferAttachments[0];
        l.descriptor = object_getClass(d);
        l.descriptorBytes = class_getInstanceSize(l.descriptor);
        if (!privateField(l.descriptor, "attachments", '@', l.attachmentsAt, l.descriptorBytes) ||
            !privateField(l.descriptor, "sampleBufferAttachments", '@', l.sampleBuffersAt, l.descriptorBytes))
            return l;
        void *array = nullptr;
        memcpy(&array, reinterpret_cast<const uint8_t *>((__bridge const void *)d) + l.attachmentsAt, sizeof array);
        if (!array)
            return l;
        l.array = object_getClass((__bridge id)array);
        ptrdiff_t colors = 0;
        if (!ivarAt(l.array, "_color_descriptors", colors) || !ivarAt(l.array, "_depth_descriptor", l.slotAt[8]) ||
            !ivarAt(l.array, "_stencil_descriptor", l.slotAt[9]))
            return l;
        for (size_t c = 0; c < 8; c++)
            l.slotAt[c] = colors + ptrdiff_t(c * sizeof(void *));
        for (size_t k = 0; k < 3; k++) {
            void *slot = nullptr;
            memcpy(&slot, static_cast<const uint8_t *>(array) + l.slotAt[k ? 7 + k : 0], sizeof slot);
            if (slot != (__bridge void *)kinds[k])
                return l;  // the array holds its attachments elsewhere than its ivars say
            l.attachment[k] = object_getClass(kinds[k]);
            l.attachmentBytes[k] = class_getInstanceSize(l.attachment[k]);
            if (!privateField(l.attachment[k], "texture", '@', l.textureAt[k], l.attachmentBytes[k]))
                return l;
        }
        void *samples = nullptr, *first = nullptr;
        memcpy(&samples, reinterpret_cast<const uint8_t *>((__bridge const void *)d) + l.sampleBuffersAt,
               sizeof samples);
        if (!samples)
            return l;
        l.samples = object_getClass((__bridge id)samples);
        l.sample = object_getClass(sample);
        if (!ivarAt(l.samples, "_sampleDescriptors", l.samplesAt) ||
            !privateField(l.sample, "sampleBuffer", '@', l.sampleBufferAt, class_getInstanceSize(l.sample)))
            return l;
        memcpy(&first, static_cast<const uint8_t *>(samples) + l.samplesAt, sizeof first);
        if (first != (__bridge void *)sample)
            return l;
        l.ok = true;
    }
    return l;
}

const Layout &layout()
{
    static const Layout l = [] {
        Layout made = makeLayout();
        if (!made.ok)
            os_log_error(rawLog(),
                         "raw: render pass descriptors' ivars are not where their encodings name them: each pass "
                         "copies and reads its descriptor");
        return made;
    }();
    return l;
}

// Two independent lanes of 8 bytes, so the multiplies of one word do not wait on the other's
uint64_t hashBytes(const uint8_t *p, size_t n)
{
    uint64_t a = 0x9e3779b97f4a7c15ull ^ n, b = 0xc2b2ae3d27d4eb4full;
    size_t at = 0;
    for (; at + 16 <= n; at += 16) {
        uint64_t x, y;
        memcpy(&x, p + at, sizeof x);
        memcpy(&y, p + at + 8, sizeof y);
        a = (a ^ x) * 0xff51afd7ed558ccdull;
        b = (b ^ y) * 0xc4ceb9fe1a85ec53ull;
        a ^= a >> 32;
        b ^= b >> 29;
    }
    for (; at + 8 <= n; at += 8) {
        uint64_t x;
        memcpy(&x, p + at, sizeof x);
        a = (a ^ x) * 0xff51afd7ed558ccdull;
        a ^= a >> 32;
    }
    return a ^ (b * 0x9e3779b97f4a7c15ull);
}

std::atomic<uint64_t> gCacheGeneration{0};

}  // namespace

struct RMPassCache {
    std::array<RMPassEntry, kEntries> entries{};
    std::array<uint64_t, kEntries> hashes{}, used{};
    uint64_t clock = 0, generation = gCacheGeneration.load(std::memory_order_acquire);
    size_t keyBytes = 0, passes = 0, textures = 0;

    ~RMPassCache()
    {
        for (RMPassEntry &e : entries)
            if (e.pass)
                CFRelease((__bridge CFTypeRef)e.pass);
    }
};

namespace {

size_t texturesOf(const RMPass &facts)
{
    size_t n = 0;
    for (const RMPass::Color &c : facts.color)
        n += (c.texture != nil) + (c.resolve != nil);
    for (const RMPass::Zeta &z : facts.zeta)
        n += z.texture != nil;
    return n + (facts.depthResolve != nil) + (facts.stencilResolve != nil);
}

CFTypeRef take(RMPassCache &c, size_t i)
{
    RMPassEntry &e = c.entries[i];
    CFTypeRef gone = (__bridge CFTypeRef)e.pass;
    --c.passes;
    c.textures -= texturesOf(e.facts);
    e.pass = nil;
    c.hashes[i] = 0;
    return gone;
}

void release(const CFTypeRef (&gone)[kEntries])
{
    for (CFTypeRef p : gone)
        if (p)
            CFRelease(p);
}

void dropIdle(RMPassCache &c)
{
    CFTypeRef gone[kEntries] = {};
    for (size_t i = 0; i < kEntries; i++)
        if (c.entries[i].pass && c.clock - c.used[i] > kIdleLookups)
            gone[i] = take(c, i);
    release(gone);
}

}  // namespace

bool rmPassKey(MTLRenderPassDescriptor *d, std::vector<uint8_t> &key, uint64_t &hash)
{
    const Layout &l = layout();
    if (!l.ok || object_getClass(d) != l.descriptor)
        return false;
    const auto *base = reinterpret_cast<const uint8_t *>((__bridge const void *)d);
    void *samples = nullptr, *array = nullptr;
    memcpy(&samples, base + l.sampleBuffersAt, sizeof samples);
    memcpy(&array, base + l.attachmentsAt, sizeof array);
    if (!array || object_getClass((__bridge id)array) != l.array)
        return false;
    if (samples) {
        if (object_getClass((__bridge id)samples) != l.samples)
            return false;
        for (size_t i = 0; i < 4; i++) {
            void *sample = nullptr, *buffer = nullptr;
            memcpy(&sample, static_cast<const uint8_t *>(samples) + l.samplesAt + ptrdiff_t(i * sizeof sample),
                   sizeof sample);
            if (!sample)
                continue;
            if (object_getClass((__bridge id)sample) != l.sample)
                return false;
            memcpy(&buffer, static_cast<const uint8_t *>(sample) + l.sampleBufferAt, sizeof buffer);
            if (buffer)
                return false;
        }
    }
    // the mask of the attachments that have a texture, the descriptor's bytes, then each of those attachments' bytes:
    // an attachment with no texture is none, whatever else it was given. Sized before it is written, so a key of the
    // last one's length is written in place.
    const size_t head = l.descriptorBytes - sizeof(void *), headPadded = (head + 7) & ~size_t(7);
    const uint8_t *attachments[kAttachments] = {};
    size_t bytes = sizeof(uint64_t) + headPadded;
    uint64_t present = 0;
    for (size_t slot = 0; slot < kAttachments; slot++) {
        const size_t kind = slot < 8 ? 0 : slot - 7;
        void *object = nullptr, *texture = nullptr;
        memcpy(&object, static_cast<const uint8_t *>(array) + l.slotAt[slot], sizeof object);
        if (!object)
            continue;
        if (object_getClass((__bridge id)object) != l.attachment[kind])
            return false;
        memcpy(&texture, static_cast<const uint8_t *>(object) + l.textureAt[kind], sizeof texture);
        if (!texture)
            continue;
        present |= uint64_t(1) << slot;
        attachments[slot] = static_cast<const uint8_t *>(object);
        bytes += (l.attachmentBytes[kind] - sizeof(void *) + 7) & ~size_t(7);
    }
    key.resize(bytes);
    uint8_t *out = key.data();
    memcpy(out, &present, sizeof present);
    out += sizeof present;
    memcpy(out, base + sizeof(void *), head);
    // the arrays' own addresses are no content
    memset(out + l.attachmentsAt - sizeof(void *), 0, sizeof(void *));
    memset(out + l.sampleBuffersAt - sizeof(void *), 0, sizeof(void *));
    memset(out + head, 0, headPadded - head);
    out += headPadded;
    for (size_t slot = 0; slot < kAttachments; slot++) {
        if (!attachments[slot])
            continue;
        const size_t kind = slot < 8 ? 0 : slot - 7;
        const size_t taken = l.attachmentBytes[kind] - sizeof(void *), padded = (taken + 7) & ~size_t(7);
        memcpy(out, attachments[slot] + sizeof(void *), taken);
        memset(out + taken, 0, padded - taken);
        out += padded;
    }
    hash = hashBytes(key.data(), key.size());
    return true;
}

RMPassEntry *rmPassFind(RMCommandBuffer *cb, const std::vector<uint8_t> &key, uint64_t hash)
{
    RMPassCache &c = *cb->_passCache;
    if (++c.clock % kEntries == 0)
        dropIdle(c);
    for (size_t i = 0; i < kEntries; i++)
        if (c.hashes[i] == hash && c.entries[i].pass && c.entries[i].key == key) {
            c.used[i] = c.clock;
            return &c.entries[i];
        }
    return nullptr;
}

void rmPassKeep(RMCommandBuffer *cb, const std::vector<uint8_t> &key, uint64_t hash, MTLRenderPassDescriptor *copy,
                const RMPass &facts, const RMRenderEncoder *e)
{
    RMPassCache &c = *cb->_passCache;
    size_t at = 0;
    for (size_t i = 0; i < kEntries; i++) {
        if (!c.entries[i].pass) {
            at = i;
            break;
        }
        if (c.used[i] < c.used[at])
            at = i;
    }
    RMPassEntry *slot = &c.entries[at];
    CFTypeRef gone = slot->pass ? take(c, at) : nullptr;
    slot->pass = (__bridge MTLRenderPassDescriptor *)CFBridgingRetain(copy);
    slot->facts = facts;
    std::copy(std::begin(e->_formats), std::end(e->_formats), slot->formats.begin());
    slot->count = e->_count;
    slot->zeta = e->_zeta;
    slot->samples = e->_samples;
    slot->turn = e->_turn;
    slot->visibility = copy.visibilityResultBuffer;
    c.keyBytes -= slot->key.capacity();
    slot->key = key;
    c.keyBytes += slot->key.capacity();
    c.hashes[at] = hash;
    c.used[at] = c.clock;
    ++c.passes;
    c.textures += texturesOf(facts);
    if (gone)
        CFRelease(gone);
}

void RMPassCacheDelete::operator()(RMPassCache *cache) const
{
    delete cache;
}

RMPassCachePtr rmNewPassCache()
{
    return RMPassCachePtr(new RMPassCache);
}

void rmPassInvalidate()
{
    gCacheGeneration.fetch_add(1, std::memory_order_seq_cst);
}

RMPassTrim rmPassCacheHeld(const RMPassCache *cache)
{
    return cache ? RMPassTrim{cache->passes, cache->textures} : RMPassTrim{};
}

RMPassTrim rmPassCacheTrim(RMPassCache *cache)
{
    const RMPassTrim held = rmPassCacheHeld(cache);
    if (cache) {
        CFTypeRef gone[kEntries] = {};
        for (size_t i = 0; i < kEntries; i++)
            if (cache->entries[i].pass)
                gone[i] = take(*cache, i);
        cache->generation = gCacheGeneration.load(std::memory_order_acquire);
        release(gone);
    }
    return held;
}

uint64_t rmPassCacheReturned(RMPassCache *cache)
{
    if (cache && cache->generation != gCacheGeneration.load(std::memory_order_acquire))
        rmPassCacheTrim(cache);
    return cache ? cache->generation : gCacheGeneration.load(std::memory_order_acquire);
}

bool rmPassCachePublished(uint64_t generation)
{
    return gCacheGeneration.fetch_add(0, std::memory_order_seq_cst) == generation;
}

size_t rmPassCacheBytes(const RMPassCache *cache)
{
    return cache ? sizeof *cache + cache->keyBytes : 0;
}
