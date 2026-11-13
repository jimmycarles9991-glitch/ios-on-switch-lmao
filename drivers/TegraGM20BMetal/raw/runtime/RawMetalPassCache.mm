// SPDX-License-Identifier: MIT
#import "RawMetalPassCache.h"
#include <objc/runtime.h>
#include <cstring>
#include "raw_objc_layout.h"

namespace {

constexpr size_t kAttachments = 10;  // 8 colour, depth, stencil
constexpr size_t kEntries = 16;
// A shape no pass took for this many lookups is let go, with the textures its copy holds: a frame's passes take theirs
// within a few
constexpr uint64_t kIdleLookups = 64;

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

size_t attachmentRoom(const Layout &l)
{
    size_t most = 0;
    for (size_t bytes : l.attachmentBytes)
        most = bytes > most ? bytes : most;
    return most;
}

uint64_t hashBytes(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n;
    for (size_t at = 0; at + 8 <= n; at += 8) {
        uint64_t w;
        memcpy(&w, p + at, sizeof w);
        h = (h ^ w) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    return h;
}

struct Cache {
    std::array<RMPassEntry, kEntries> entries;
    uint64_t clock = 0;
};

thread_local Cache tCache;

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
    const size_t head = l.descriptorBytes - sizeof(void *), room = attachmentRoom(l);
    key.resize((head + kAttachments * (room + 8) + 7) & ~size_t(7));
    uint8_t *out = key.data();
    memcpy(out, base + sizeof(void *), head);
    // the arrays' own addresses are no content
    memset(out + l.attachmentsAt - sizeof(void *), 0, sizeof(void *));
    memset(out + l.sampleBuffersAt - sizeof(void *), 0, sizeof(void *));
    out += head;
    for (size_t slot = 0; slot < kAttachments; slot++, out += room + 8) {
        const size_t kind = slot < 8 ? 0 : slot - 7;
        void *object = nullptr, *texture = nullptr;
        memcpy(&object, static_cast<const uint8_t *>(array) + l.slotAt[slot], sizeof object);
        const auto *a = static_cast<const uint8_t *>(object);
        if (a && object_getClass((__bridge id)object) != l.attachment[kind])
            return false;
        if (a)
            memcpy(&texture, a + l.textureAt[kind], sizeof texture);
        // an attachment with no texture is none, whatever else it was given
        memset(out, 0, room + 8);
        if (texture) {
            out[0] = 1;
            memcpy(out + 8, a + sizeof(void *), l.attachmentBytes[kind] - sizeof(void *));
        }
    }
    hash = hashBytes(key.data(), key.size());
    return true;
}

RMPassEntry *rmPassFind(const std::vector<uint8_t> &key, uint64_t hash)
{
    Cache &c = tCache;
    c.clock++;
    RMPassEntry *found = nullptr;
    for (RMPassEntry &e : c.entries) {
        if (!e.pass)
            continue;
        if (!found && e.hash == hash && e.key == key) {
            e.used = c.clock;
            found = &e;
        } else if (c.clock - e.used > kIdleLookups) {
            e.pass = nil;
        }
    }
    return found;
}

void rmPassKeep(const std::vector<uint8_t> &key, uint64_t hash, MTLRenderPassDescriptor *copy, const RMPass &facts,
                const RMRenderEncoder *e)
{
    Cache &c = tCache;
    RMPassEntry *slot = &c.entries[0];
    for (RMPassEntry &entry : c.entries) {
        if (!entry.pass) {
            slot = &entry;
            break;
        }
        if (entry.used < slot->used)
            slot = &entry;
    }
    slot->pass = copy;
    slot->facts = facts;
    std::copy(std::begin(e->_formats), std::end(e->_formats), slot->formats.begin());
    slot->count = e->_count;
    slot->zeta = e->_zeta;
    slot->samples = e->_samples;
    slot->turn = e->_turn;
    slot->visibility = copy.visibilityResultBuffer;
    slot->hash = hash;
    slot->used = c.clock;
    slot->key = key;
}
