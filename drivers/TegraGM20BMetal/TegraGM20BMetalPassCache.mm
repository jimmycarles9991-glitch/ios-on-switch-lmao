// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalPassCache.h"
#include <objc/runtime.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include "objc_layout.h"

namespace {

constexpr size_t kAttachments = 10;  // 8 colour, depth, stencil
// a compositor frame's passes, each its own shape (boot 21: 29-40 render targets a command buffer), kept to the next
constexpr size_t kEntries = 64;
// A shape no pass took for this many lookups is let go: a frame's passes take theirs within a few
constexpr uint64_t kIdleLookups = 4 * kEntries;
static_assert(kEntries <= 64, "SwitchMetalPassCache::bound has a bit an entry");

constexpr size_t kObjects = 8;

// The objects an instance of one of Metal's descriptor classes holds, in its ivars or its `_private` struct: a client's
// (a texture, resolve texture or visibility buffer), which a key names by its identity, or another, which must be nil
struct Objects {
    ptrdiff_t at[kObjects] = {};
    bool client[kObjects] = {};
    size_t count = 0;
};

// Where Metal's descriptor classes keep what a pass reads, from their ivars' names and encodings
struct Layout {
    bool ok = false;
    Class descriptor = nil, array = nil, attachment[3] = {}, samples = nil, sample = nil, texture = nil, buffer = nil;
    Objects head, objects[3];  // the descriptor's but its two arrays, and each attachment kind's
    size_t descriptorBytes = 0, attachmentBytes[3] = {};
    ptrdiff_t attachmentsAt = 0, sampleBuffersAt = 0;  // in the descriptor
    ptrdiff_t slotAt[kAttachments] = {};               // each attachment's object in the attachment array
    ptrdiff_t textureAt[3] = {};                       // an attachment's texture in its object
    ptrdiff_t samplesAt = 0, sampleBufferAt = 0;       // the sample buffer attachments in their array, a buffer in one
};

bool privateFields(Class cls, size_t instance, std::vector<gm20b::objc::Field> &fields, ptrdiff_t &base)
{
    Ivar ivar = class_getInstanceVariable(cls, "_private");
    const char *type = ivar ? ivar_getTypeEncoding(ivar) : nullptr;
    size_t size = 0;
    if (!type || !gm20b::objc::structFields(type, fields, size) || size_t(ivar_getOffset(ivar)) + size > instance)
        return false;
    base = ivar_getOffset(ivar);
    return true;
}

bool privateField(Class cls, const char *field, char kind, ptrdiff_t &at, size_t instance)
{
    std::vector<gm20b::objc::Field> fields;
    ptrdiff_t base = 0;
    if (!privateFields(cls, instance, fields, base))
        return false;
    const gm20b::objc::Field *f = gm20b::objc::field(fields, field, kind);
    if (!f)
        return false;
    at = base + ptrdiff_t(f->offset);
    return true;
}

bool objectsOf(Class cls, size_t instance, std::string_view skip0, std::string_view skip1, Objects &o)
{
    const auto add = [&](std::string_view name, ptrdiff_t at) {
        if (!name.empty() && name.front() == '_')
            name.remove_prefix(1);
        if ((!skip0.empty() && name == skip0) || (!skip1.empty() && name == skip1))
            return true;
        if (o.count == kObjects)
            return false;
        o.at[o.count] = at;
        o.client[o.count++] = name == "texture" || name == "resolveTexture" || name == "visibilityResultBuffer";
        return true;
    };
    for (Class c = cls; c && c != [NSObject class]; c = class_getSuperclass(c)) {
        unsigned n = 0;
        Ivar *ivars = class_copyIvarList(c, &n);
        bool ok = true;
        for (unsigned i = 0; i < n && ok; i++) {
            const char *type = ivar_getTypeEncoding(ivars[i]);
            if (type && type[0] == '@')
                ok = add(ivar_getName(ivars[i]), ivar_getOffset(ivars[i]));
        }
        free(ivars);
        if (!ok)
            return false;
    }
    std::vector<gm20b::objc::Field> fields;
    ptrdiff_t base = 0;
    if (!privateFields(cls, instance, fields, base))
        return false;
    for (const gm20b::objc::Field &f : fields)
        if (f.kind == '@' && !add(f.name, base + ptrdiff_t(f.offset)))
            return false;
    return true;
}

// Whether `object` holds no object but, where `clients`, its clients'
bool holdsOnly(const void *object, const Objects &o, bool clients)
{
    for (size_t i = 0; i < o.count; i++) {
        void *p = nullptr;
        memcpy(&p, static_cast<const uint8_t *>(object) + o.at[i], sizeof p);
        if (p && (!o.client[i] || !clients))
            return false;
    }
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
        l.texture = [SwitchMetalTexture class];
        l.buffer = [SwitchMetalBuffer class];
        if (!privateField(l.descriptor, "attachments", '@', l.attachmentsAt, l.descriptorBytes) ||
            !privateField(l.descriptor, "sampleBufferAttachments", '@', l.sampleBuffersAt, l.descriptorBytes) ||
            !objectsOf(l.descriptor, l.descriptorBytes, "attachments", "sampleBufferAttachments", l.head) ||
            !holdsOnly((__bridge const void *)d, l.head, true))
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
            if (!privateField(l.attachment[k], "texture", '@', l.textureAt[k], l.attachmentBytes[k]) ||
                !objectsOf(l.attachment[k], l.attachmentBytes[k], {}, {}, l.objects[k]) ||
                !holdsOnly(slot, l.objects[k], true))
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
            os_log_error(
                gm20bLog(),
                "SwitchMetal: render pass descriptors' ivars are not where their encodings name them: each pass "
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

// `object`'s objects where its key holds them (`out` at `object` past its class pointer): none as 0, a client's texture or
// buffer of this device as its identity; false where it holds anything else
bool keyObjects(const Layout &l, const uint8_t *object, const Objects &o, uint8_t *out)
{
    for (size_t i = 0; i < o.count; i++) {
        void *p = nullptr;
        memcpy(&p, object + o.at[i], sizeof p);
        uint64_t identity = 0;
        if (p) {
            const Class c = object_getClass((__bridge id)p);
            if (!o.client[i] || (c != l.texture && c != l.buffer))
                return false;
            __unsafe_unretained SwitchMetalObject *named = (__bridge SwitchMetalObject *)p;
            identity = named->_identity;
            if (!identity)
                return false;
        }
        memcpy(out + o.at[i] - ptrdiff_t(sizeof(void *)), &identity, sizeof identity);
    }
    return true;
}

}  // namespace

struct SwitchMetalPassCache {
    std::array<SwitchMetalPassEntry, kEntries> entries{};
    std::array<uint64_t, kEntries> hashes{}, used{};
    uint64_t clock = 0, since = 0;  // since: the clock its last command buffer returned it at
    uint64_t bound =
        0;  // the entries whose copies name their textures: those its command buffer kept or gm20bPassBind named
    size_t keyBytes = 0, passes = 0, textures = 0;  // textures: the bound entries'

    ~SwitchMetalPassCache()
    {
        for (SwitchMetalPassEntry &e : entries)
            if (e.pass)
                CFRelease((__bridge CFTypeRef)e.pass);
    }
};

namespace {

size_t texturesOf(const SwitchMetalPass &facts)
{
    size_t n = 0;
    for (const SwitchMetalPass::Color &c : facts.color)
        n += (c.texture != nil) + (c.resolve != nil);
    for (const SwitchMetalPass::Zeta &z : facts.zeta)
        n += z.texture != nil;
    return n + (facts.depthResolve != nil) + (facts.stencilResolve != nil);
}

// The client objects `e`'s copy holds, named again for its command buffer (each alive: that command buffer holds what its
// pass names) or let go: one message each, its attachments' objects read where gm20bPassKey reads them. Named again, a depth
// or stencil resolve is only one the pass resolves into, as the command buffer holds no other.
void name(const SwitchMetalPassEntry &e, bool named)
{
    const Layout &l = layout();
    const uint8_t *array = nullptr;
    memcpy(&array, reinterpret_cast<const uint8_t *>((__bridge const void *)e.pass) + l.attachmentsAt, sizeof array);
    const auto attachment = [&](size_t slot) {
        void *object = nullptr;
        memcpy(&object, array + l.slotAt[slot], sizeof object);
        return object;
    };
    for (unsigned c = 0; c < 8; c++)
        if (e.facts.color[c].texture)
            [(__bridge MTLRenderPassAttachmentDescriptor *)attachment(c)
                setTexture:named ? e.facts.color[c].texture : nil];
    for (unsigned i = 0; i < 2; i++) {
        if (e.facts.zeta[i].texture)
            [(__bridge MTLRenderPassAttachmentDescriptor *)attachment(8 + i)
                setTexture:named ? e.facts.zeta[i].texture : nil];
        __unsafe_unretained id resolve = i ? e.facts.stencilResolve : e.facts.depthResolve;
        if (named ? resolve != nil : e.zetaResolve[i] != nil)
            [(__bridge MTLRenderPassAttachmentDescriptor *)attachment(8 + i) setResolveTexture:named ? resolve : nil];
    }
    if (e.visibility)
        [e.pass setVisibilityResultBuffer:named ? e.visibility : nil];
}

void bind(SwitchMetalPassCache &c, size_t i)
{
    if (c.bound >> i & 1)
        return;
    name(c.entries[i], true);
    c.bound |= uint64_t(1) << i;
    c.textures += texturesOf(c.entries[i].facts);
}

// Let go by the bank. A copy its command buffer found goes on as that command buffer's pass: it names its textures again
// first.
CFTypeRef take(SwitchMetalPassCache &c, size_t i)
{
    SwitchMetalPassEntry &e = c.entries[i];
    if (c.used[i] > c.since)
        bind(c, i);
    CFTypeRef gone = (__bridge CFTypeRef)e.pass;
    --c.passes;
    if (c.bound >> i & 1)
        c.textures -= texturesOf(e.facts);
    c.bound &= ~(uint64_t(1) << i);
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

void dropIdle(SwitchMetalPassCache &c)
{
    CFTypeRef gone[kEntries] = {};
    for (size_t i = 0; i < kEntries; i++)
        if (c.entries[i].pass && c.clock - c.used[i] > kIdleLookups)
            gone[i] = take(c, i);
    release(gone);
}

}  // namespace

bool gm20bPassKey(MTLRenderPassDescriptor *d, std::vector<uint8_t> &key, uint64_t &hash)
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
        if (!texture) {
            if (!holdsOnly(object, l.objects[kind], false))
                return false;  // a resolve texture with no texture, which a kept copy would hold past its command buffer
            continue;
        }
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
    if (!keyObjects(l, base, l.head, out))
        return false;
    out += headPadded;
    for (size_t slot = 0; slot < kAttachments; slot++) {
        if (!attachments[slot])
            continue;
        const size_t kind = slot < 8 ? 0 : slot - 7;
        const size_t taken = l.attachmentBytes[kind] - sizeof(void *), padded = (taken + 7) & ~size_t(7);
        memcpy(out, attachments[slot] + sizeof(void *), taken);
        memset(out + taken, 0, padded - taken);
        if (!keyObjects(l, attachments[slot], l.objects[kind], out))
            return false;
        out += padded;
    }
    hash = hashBytes(key.data(), key.size());
    return true;
}

SwitchMetalPassEntry *gm20bPassFind(SwitchMetalCommandBuffer *cb, const std::vector<uint8_t> &key, uint64_t hash)
{
    SwitchMetalPassCache &c = *cb->_passCache;
    if (++c.clock % kEntries == 0)
        dropIdle(c);
    for (size_t i = 0; i < kEntries; i++)
        if (c.hashes[i] == hash && c.entries[i].pass && c.entries[i].key == key) {
            c.used[i] = c.clock;
            return &c.entries[i];
        }
    return nullptr;
}

void gm20bPassKeep(SwitchMetalCommandBuffer *cb, const std::vector<uint8_t> &key, uint64_t hash,
                   MTLRenderPassDescriptor *copy, const SwitchMetalPass &facts, const SwitchMetalRenderEncoder *e)
{
    SwitchMetalPassCache &c = *cb->_passCache;
    size_t at = 0;
    for (size_t i = 0; i < kEntries; i++) {
        if (!c.entries[i].pass) {
            at = i;
            break;
        }
        if (c.used[i] < c.used[at])
            at = i;
    }
    SwitchMetalPassEntry *slot = &c.entries[at];
    CFTypeRef gone = slot->pass ? take(c, at) : nullptr;
    slot->pass = (__bridge MTLRenderPassDescriptor *)CFBridgingRetain(copy);
    slot->facts = facts;
    std::copy(std::begin(e->_formats), std::end(e->_formats), slot->formats.begin());
    slot->count = e->_count;
    slot->zeta = e->_zeta;
    slot->samples = e->_samples;
    slot->turn = e->_turn;
    slot->visibility = copy.visibilityResultBuffer;
    slot->zetaResolve[0] = copy.depthAttachment.resolveTexture;
    slot->zetaResolve[1] = copy.stencilAttachment.resolveTexture;
    c.keyBytes -= slot->key.capacity();
    slot->key = key;
    c.keyBytes += slot->key.capacity();
    c.hashes[at] = hash;
    c.used[at] = c.clock;
    ++c.passes;
    c.bound |= uint64_t(1) << at;
    c.textures += texturesOf(facts);
    if (gone)
        CFRelease(gone);
}

void SwitchMetalPassCacheDelete::operator()(SwitchMetalPassCache *cache) const
{
    delete cache;
}

SwitchMetalPassCachePtr gm20bNewPassCache()
{
    return SwitchMetalPassCachePtr(new SwitchMetalPassCache);
}

void gm20bPassCacheUnbind(SwitchMetalPassCache *cache)
{
    if (!cache)
        return;
    for (uint64_t bound = cache->bound; bound; bound &= bound - 1)
        name(cache->entries[size_t(__builtin_ctzll(bound))], false);
    cache->bound = 0;
    cache->textures = 0;
    cache->since = cache->clock;
}

void gm20bPassBind(SwitchMetalCommandBuffer *cb, MTLRenderPassDescriptor *pass)
{
    SwitchMetalPassCache *c = cb->_passCache.get();
    if (!c || !pass)
        return;
    for (size_t i = 0; i < kEntries; i++)
        if (c->entries[i].pass == pass) {
            bind(*c, i);
            return;
        }
}

SwitchMetalPassTrim gm20bPassCacheHeld(const SwitchMetalPassCache *cache)
{
    return cache ? SwitchMetalPassTrim{cache->passes, cache->textures} : SwitchMetalPassTrim{};
}

SwitchMetalPassTrim gm20bPassCacheTrim(SwitchMetalPassCache *cache)
{
    const SwitchMetalPassTrim held = gm20bPassCacheHeld(cache);
    if (cache) {
        CFTypeRef gone[kEntries] = {};
        for (size_t i = 0; i < kEntries; i++)
            if (cache->entries[i].pass)
                gone[i] = take(*cache, i);
        release(gone);
    }
    return held;
}

size_t gm20bPassCacheBytes(const SwitchMetalPassCache *cache)
{
    return cache ? sizeof *cache + cache->keyBytes : 0;
}
