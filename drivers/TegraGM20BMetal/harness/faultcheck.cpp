// SPDX-License-Identifier: MIT
// raw GM20B path, over the mock GPU (its decoder: what reached the GPU): what the device hands the GPU stays valid.
// - A view frees its own texture header alone, never what its parent owns (a planar or YCbCr texture's planes, a stand-in), and
//   a converted texture has no raw view. Build it with -fsanitize=address: the old view freed its parent's planes, which the
//   parent then used and freed again.
// - An instanced draw of thousands of instances stays in its push reservations (test/build.sh's harness builds stop a push
//   that ran past one) and reaches the GPU whole.
// - A draw whose vertex buffer is unbound fetches from NVK's empty stream (the zero page, its limit before its start), never
//   from VA 0 with no limit.
// - A pass with an empty color slot below a target disables the slot, as NVK does, and counts through the target.
// - A freed texture header is the null descriptor, never zeroes (a buffer texture at VA 0).
// - A memory barrier inside a pass flushes the shaders' data writes, as the barrier between passes does.
// - A buffer the copy engine fills is read as a constant buffer by a later pass of the same command buffer behind a
//   constant-cache invalidate (nvdec's stale constant reads: none).
// - A commit whose later Exec the kernel refuses leaves nothing of its rings in flight, and the next command buffer a new
//   channel.
// - A wait that fails on a channel destroyed meanwhile marks no new channel of the same id lost.
// - A command buffer larger than its push ring reaches the GPU whole (the ring grows under it).
// - A fill writes its byte over its range, at any alignment and length, and nothing around it.
// - A channel the kernel killed is made again with its context before any command buffer is encoded for it: one whose
//   ChannelCreate or context Exec the kernel refused gives no command buffer, nor does a begin() after it, until a channel
//   destroyed elsewhere moves the kernel's channel epoch; the begin() after that asks once.
// - Programs placed in the shader heap are behind an instruction-cache invalidate before any draw runs them, even when the
//   kernel refused the submission whose prologue was to invalidate it.
// - A fence wait the kernel refuses with no kill is no completion: it is waited again, and one the kernel keeps refusing is a
//   failure (the GPU runs each submission for MOCK_GPU_US, so it is in flight when waited for).
// - A client whose first channel the kernel refuses says why, asks for none again (a second later neither) until a channel
//   destroyed elsewhere moves the kernel's channel epoch, and then gets one.
// usage: faultcheck <pack.bin>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <string>
#include <tuple>
#include <unistd.h>
#include "../gpu.h"
#include "TegraGM20BJobABI.h"

extern "C" uint64_t cap_draws(void);
extern "C" uint32_t cap_reg3d(uint32_t mthd);
extern "C" bool cap_read(uint64_t va, void *out, uint64_t bytes);
extern "C" uint64_t cap_instruction_invalidates(void);
extern "C" uint64_t cap_shader_invalidates(void);
extern "C" uint64_t cap_constant_invalidates(void);
extern "C" uint64_t cap_stale_constant_reads(void);
extern "C" void mock_iokit_kill_next(uint32_t error);
extern "C" void mock_iokit_fail_next(unsigned method, unsigned count, int kr);
extern "C" void mock_iokit_fail_after(unsigned method, unsigned skip, unsigned count, int kr);
extern "C" unsigned long mock_iokit_calls(unsigned method);

namespace {

using Expect = std::function<void(bool, const char *)>;

constexpr uint32_t kBGRA8 = 80, kBGRA8sRGB = 81, kR8 = 10;

void views(gm20b::Device &dev, const Expect &expect)
{
    gm20b::Texture parent;
    if (!dev.createTexture(64, 64, kBGRA8, parent)) {
        expect(false, "a texture to view");
        return;
    }
    // what a converted texture owns, by hand: no host pack has the conversion states that make one
    parent.luma = new gm20b::Texture;
    parent.standIn = new gm20b::Texture;
    expect(dev.createTexture(64, 64, kR8, *parent.luma) && dev.createTexture(64, 64, kBGRA8, *parent.standIn),
           "the parent's plane and stand-in");
    const uint32_t lumaTic = parent.luma->tic;
    gm20b::Texture view;
    expect(dev.createView(parent, kBGRA8sRGB, view) && view.tic && view.tic != parent.tic,
           "a view with its own header");
    dev.destroyTexture(view);
    expect(parent.luma && parent.luma->tic == lumaTic && parent.standIn && parent.standIn->tic,
           "the view's end left the parent's plane and stand-in alone");
    gm20b::Texture again;
    expect(dev.createView(parent, kBGRA8sRGB, again), "the parent viewed again");
    dev.destroyTexture(again);
    parent.planar = true;
    expect(!dev.createView(parent, kBGRA8sRGB, again),
           "a planar texture has no raw view (Metal views it as 542 alone)");
    parent.planar = false;
    parent.ycbcr = 1;
    expect(!dev.createView(parent, kBGRA8sRGB, again), "a YCbCr texture has no raw view");
    parent.ycbcr = 0;
    dev.destroyTexture(parent);
}

// A pass over targets of the formats of a pack state that fetches vertices and tests no depth (`ubo`: and reads a uniform
// buffer as a constant buffer), and that state
struct Pass {
    gm20b::Pipeline *state = nullptr;
    gm20b::Texture targets[8];
    gm20b::PassDesc desc;

    bool make(gm20b::Device &dev, bool ubo = false)
    {
        const gm20b::PackPipeline *recs = dev.pack().section<gm20b::PackPipeline>(gm20b::PackPipelines);
        for (uint32_t i = 0; i < dev.pack().header()->pipelines && !state; i++)
            if (gm20b::Pipeline *p = dev.pipeline(recs[i].keyHash);
                p && !p->depthFormat && p->rec->vertexSlot < 31 && (!ubo || p->uboCount))
                state = p;
        if (!state)
            return false;
        for (uint32_t j = 0; j < 8; j++)
            if (const uint32_t f = state->colorFormats[j]) {
                if (!dev.createTexture(64, 64, f, targets[j]))
                    return false;
                desc.color[j].texture = &targets[j];
                desc.count = j + 1;
            }
        return desc.count != 0;
    }
};

void instances(gm20b::Device &dev, Pass &pass, const Expect &expect)
{
    constexpr uint32_t kInstances = 4000;
    gm20b::Buffer indices;
    if (!dev.createBuffer(64, indices)) {
        expect(false, "an index buffer");
        return;
    }
    const uint16_t tri[3] = {0, 1, 2};
    memcpy(indices.cpu, tri, sizeof tri);
    const uint64_t before = cap_draws();
    gm20b::CommandBuffer *cb = dev.begin();
    gm20b::RenderEncoder *e = cb ? cb->render(pass.desc) : nullptr;
    const char *why = nullptr;
    bool drawn = false;
    if (e) {
        e->setPipeline(pass.state);
        drawn = e->draw(3, 0, 3, kInstances, 0, &why) && e->drawIndexed(3, 3, 2, &indices, 0, kInstances, 0, 0, &why);
        e->end();
    }
    uint32_t seq = 0;
    expect(drawn && cb->commit(seq) && dev.wait(seq), "a draw and an indexed draw of 4000 instances each, submitted");
    expect(cap_draws() - before == 2 * kInstances, "every instance of both reached the GPU");
    dev.destroyBuffer(indices);
}

// A pass whose first color slot is empty below a target: the slot is DISABLED and CT_SELECT counts through the target, never
// the earlier pass's first target still in the slot
void holeTarget(gm20b::Device &dev, Pass &pass, const Expect &expect)
{
    constexpr uint32_t kTargetFormat0 = 0x0810, kCtSelect = 0x121c;  // SET_COLOR_TARGET_FORMAT(0), SET_CT_SELECT
    if (pass.desc.count < 2) {
        expect(false, "a state of two color targets");
        return;
    }
    auto run = [&](const gm20b::PassDesc &desc) {
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::RenderEncoder *e = cb ? cb->render(desc) : nullptr;
        if (!e)
            return false;
        e->end();
        uint32_t seq = 0;
        return cb->commit(seq) && dev.wait(seq);
    };
    gm20b::PassDesc hole = pass.desc;
    hole.color[0].texture = nullptr;
    expect(run(pass.desc) && cap_reg3d(kTargetFormat0) != 0, "a pass over both targets");
    expect(run(hole), "a pass over the second alone");
    std::printf("hole: target 0 format 0x%x, CT_SELECT 0x%x\n", cap_reg3d(kTargetFormat0), cap_reg3d(kCtSelect));
    expect(cap_reg3d(kTargetFormat0) == 0 && (cap_reg3d(kCtSelect) & 0xf) == 2,
           "slot 0 DISABLED, CT_SELECT counting through slot 1");
}

// A memory barrier between two draws of a pass flushes what the shaders wrote (INVALIDATE_SHADER_CACHES FLUSH_DATA): one
// more than the same pass without it
void barrierFlush(gm20b::Device &dev, Pass &pass, const Expect &expect)
{
    auto run = [&](bool barrier) -> uint64_t {
        const uint64_t before = cap_shader_invalidates();
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::RenderEncoder *e = cb ? cb->render(pass.desc) : nullptr;
        if (!e)
            return ~0ull;
        const char *why = nullptr;
        e->setPipeline(pass.state);
        e->draw(3, 0, 3, 1, 0, &why);
        if (barrier)
            e->barrier();
        e->draw(3, 0, 3, 1, 0, &why);
        e->end();
        uint32_t seq = 0;
        return cb->commit(seq) && dev.wait(seq) ? cap_shader_invalidates() - before : ~0ull;
    };
    const uint64_t plain = run(false), barrier = run(true);
    std::printf("shader cache flushes: %llu without the barrier, %llu with\n", (unsigned long long)plain,
                (unsigned long long)barrier);
    expect(plain != ~0ull && barrier == plain + 1, "the barrier flushes the shaders' data");
}

// The copy engine fills a buffer, then a pass's draw reads it as a constant buffer: the submission's prologue invalidated the
// constant caches before the fill, the pass's prologue does after it
void constantAfterFill(gm20b::Device &dev, const Expect &expect)
{
    Pass pass;
    gm20b::Buffer b;
    if (!pass.make(dev, true) || !dev.createBuffer(4096, b)) {
        expect(false, "a state reading a uniform buffer as a constant buffer, and a buffer");
        return;
    }
    const uint64_t invalidates = cap_constant_invalidates(), stale = cap_stale_constant_reads();
    gm20b::CommandBuffer *cb = dev.begin();
    bool drawn = false;
    if (cb) {
        cb->use(b, true, false);
        gm20b::BlitEncoder *fill = cb->blit();
        fill->fillBytes(b.va, 4096, 0x5a);
        fill->end();
        if (gm20b::RenderEncoder *e = cb->render(pass.desc)) {
            const char *why = nullptr;
            e->setPipeline(pass.state);
            for (uint8_t u = 0; u < pass.state->uboCount; u++) {
                const gm20b::PackEntry &x = pass.state->rec->entry[pass.state->ubos[u].entry];
                e->setBuffer(x.stage, x.slot, &b, 0);
            }
            drawn = e->draw(3, 0, 3, 1, 0, &why);
            e->end();
        }
    }
    uint32_t seq = 0;
    expect(drawn && cb->commit(seq) && dev.wait(seq), "a fill, then a draw reading the buffer as a constant buffer");
    const uint64_t n = cap_constant_invalidates() - invalidates, s = cap_stale_constant_reads() - stale;
    std::printf("constant-cache invalidates %llu, stale constant reads %llu\n", (unsigned long long)n,
                (unsigned long long)s);
    expect(n == 2 && s == 0, "the constant caches invalidated after the fill, before the draw");
    dev.destroyBuffer(b);
}

uint64_t method64(uint32_t hi)
{
    return uint64_t(cap_reg3d(hi)) << 32 | cap_reg3d(hi + 4);
}

// A freed texture header (a texture's, a view's) reads as header 0, the null descriptor, never as zeroes (a buffer texture at
// VA 0)
void freedHeader(gm20b::Device &dev, const Expect &expect)
{
    constexpr uint32_t kHeaderPool = 0x1574;  // SET_TEX_HEADER_POOL_A
    const uint64_t pool = method64(kHeaderPool);
    gm20b::Texture t, v;
    if (!pool || !dev.createTexture(64, 64, kBGRA8, t) || !dev.createView(t, kBGRA8sRGB, v)) {
        expect(false, "the header pool, a texture and a view");
        return;
    }
    const uint32_t tic = t.tic, view = v.tic;
    dev.destroyTexture(v);
    dev.destroyTexture(t);
    uint32_t null[8], freed[8], freedView[8];
    expect(cap_read(pool, null, 32) && cap_read(pool + 32 * tic, freed, 32) &&
               cap_read(pool + 32 * view, freedView, 32),
           "the headers read back");
    std::printf("freed header %u: %08x %08x %08x; null %08x %08x %08x\n", tic, freed[0], freed[1], freed[2], null[0],
                null[1], null[2]);
    expect(null[0] && !memcmp(freed, null, 32) && !memcmp(freedView, null, 32),
           "the freed texture's and view's headers are the null descriptor");
}

void unboundStream(gm20b::Device &dev, Pass &pass, const Expect &expect)
{
    constexpr uint32_t kStreamLocation = 0x1c04,
                       kStreamLimit = 0x1f00;  // SET_VERTEX_STREAM_A_LOCATION_A, _LIMIT_A_A(0)
    auto drawWith = [&](const gm20b::Buffer *vertices) {
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::RenderEncoder *e = cb ? cb->render(pass.desc) : nullptr;
        if (!e)
            return false;
        e->setPipeline(pass.state);
        e->setBuffer(1, pass.state->rec->vertexSlot, vertices, 0);
        const char *why = nullptr;
        const bool drawn = e->draw(3, 0, 3, 1, 0, &why);
        e->end();
        uint32_t seq = 0;
        return drawn && cb->commit(seq) && dev.wait(seq);
    };
    expect(drawWith(nullptr), "a draw with its vertex buffer unbound");
    uint64_t location = method64(kStreamLocation), limit = method64(kStreamLimit);
    std::printf("unbound stream: location 0x%llx (%s), limit 0x%llx\n", (unsigned long long)location,
                dev.vaOwner(location).c_str(), (unsigned long long)limit);
    expect(dev.vaOwner(location) == "the zero page +0x0" && limit == location - 1,
           "it fetches from NVK's empty stream: the zero page, its limit before its start");
    gm20b::Buffer vertices;
    expect(dev.createBuffer(4096, vertices) && drawWith(&vertices), "a draw with its vertex buffer bound");
    location = method64(kStreamLocation);
    limit = method64(kStreamLimit);
    expect(location == vertices.va && limit == vertices.va + 4095, "it fetches the buffer's bytes");
    dev.destroyBuffer(vertices);
}

// One copy in a command buffer of its own, committed: its seqno, 0 when the device gave no command buffer or the commit failed
uint32_t copy(gm20b::Device &dev, const gm20b::Buffer &from, const gm20b::Buffer &to)
{
    gm20b::CommandBuffer *cb = dev.begin();
    if (!cb)
        return 0;
    cb->use(from, false);
    cb->use(to, true, false);
    gm20b::BlitEncoder *e = cb->blit();
    e->copyBytes(from.va, to.va, from.length);
    e->end();
    uint32_t seqno = 0;
    if (!cb->commit(seqno)) {
        cb->abandon();
        return 0;
    }
    return seqno;
}

// A command buffer of more segments than one Exec takes, whose second Exec the kernel refuses: the first batch has completed
// when commit returns (abandon gives its ring words back), and the next command buffer runs on a new channel with its context
void partialCommit(gm20b::Device &dev, const Expect &expect)
{
    gm20b::Buffer a, b;
    if (!dev.createBuffer(4096, a) || !dev.createBuffer(4096, b)) {
        expect(false, "two buffers");
        return;
    }
    gm20b::CommandBuffer *cb = dev.begin();
    if (!cb) {
        expect(false, "a command buffer");
        return;
    }
    cb->use(a, false);
    cb->use(b, true, false);
    for (int i = 0; i < 300; i++) {
        gm20b::BlitEncoder *e = cb->blit();
        e->copyBytes(a.va, b.va, 64);
        e->end();
    }
    const uint64_t reopens = dev.stats.reopens.load();
    mock_iokit_fail_after(TegraGM20BMethodSubmitJob, 1, 1, kIOReturnNoMemory);
    uint32_t seq = 0;
    expect(!cb->commit(seq), "the commit fails at its second SubmitJob");
    const bool settled = dev.done(dev.submitted());
    cb->abandon();
    expect(settled, "its first batch completed before commit returned");
    const uint32_t next = copy(dev, a, b);
    expect(next && dev.wait(next) && dev.stats.reopens.load() == reopens + 1,
           "the next command buffer ran on a new channel");
    dev.destroyBuffer(a);
    dev.destroyBuffer(b);
}

// A wait on a channel that answers NotResponding only after the channel was destroyed and its id given to a new one (a
// completion thread's wait across the submit thread's reopen) marks nothing lost: the new channel is not the dead one
void lostReused(gm20b::Kernel &k, const Expect &expect)
{
    uint32_t first = 0, second = 0;
    const volatile uint32_t *fence = nullptr;
    if (!k.channel(1, first, fence)) {
        expect(false, "a channel");
        return;
    }
    std::thread waiter([&] {
        k.wait(first, 1000);  // never reached: answers when the channel's slot is made again
    });
    usleep(20000);
    k.closeChannel(first);
    expect(k.channel(1, second, fence) && second == first, "a new channel in the first one's slot");
    waiter.join();
    expect(!k.lost(second), "the new channel is not lost");
    k.closeChannel(second);
}

// A command buffer larger than its push ring (a client's: 256 KiB): 10000 copies, each a word on from the last, in one blit
// encoder. Its words are all there when it is submitted, so the word at the end is the first one's
void outgrownRing(const char *pack, const Expect &expect)
{
    constexpr uint32_t kCopies = 10000;
    gm20b::Device client;
    std::string err;
    gm20b::Buffer chain;
    if (!client.open(pack, 0, err, gm20b::Device::Role::Client) || !client.createBuffer((kCopies + 1) * 4, chain)) {
        expect(false, "a client device and a buffer");
        return;
    }
    memset(chain.cpu, 0, chain.length);
    reinterpret_cast<uint32_t *>(chain.cpu)[0] = 0x5eed1234;
    gm20b::CommandBuffer *cb = client.begin();
    if (!cb) {
        expect(false, "a command buffer");
        return;
    }
    cb->clean(chain.cpu, chain.length);
    cb->use(chain, true, false);
    gm20b::BlitEncoder *e = cb->blit();
    for (uint32_t i = 0; i < kCopies; i++)
        e->copyBytes(chain.va + 4 * i, chain.va + 4 * (i + 1), 4);
    e->end();
    uint32_t seq = 0;
    expect(cb->commit(seq) && client.wait(seq), "submitted and completed");
    const uint32_t last = reinterpret_cast<const uint32_t *>(chain.cpu)[kCopies];
    std::printf("outgrown ring: the last word 0x%08x\n", last);
    expect(last == 0x5eed1234, "every copy ran, in order");
    client.destroyBuffer(chain);
}

// Fills of a byte value over ranges of every alignment, one of them past the copy engine's line (32768 words): the range
// holds the value, every byte around it what it held
void fills(gm20b::Device &dev, const Expect &expect)
{
    constexpr uint64_t kBytes = 400000;
    gm20b::Buffer b;
    if (!dev.createBuffer(kBytes, b)) {
        expect(false, "a buffer");
        return;
    }

    const struct {
        uint64_t at, bytes;
        uint8_t value;
    } cases[] = {{3, 1, 0x11}, {1, 2, 0x22}, {9, 4, 0x33}, {16, 64, 0x44}, {101, 7, 0x55}, {1001, 300001, 0x66}};

    bool all = true;
    for (const auto &f : cases) {
        memset(b.cpu, 0xee, kBytes);
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb) {
            all = false;
            break;
        }
        cb->clean(b.cpu, kBytes, true);
        cb->use(b, true, false);
        gm20b::BlitEncoder *e = cb->blit();
        e->fillBytes(b.va + f.at, f.bytes, f.value);
        e->end();
        uint32_t seq = 0;
        if (!cb->commit(seq) || !dev.wait(seq)) {
            all = false;
            break;
        }
        uint64_t wrong = 0;
        for (uint64_t i = 0; i < kBytes; i++)
            wrong += b.cpu[i] != (i >= f.at && i < f.at + f.bytes ? f.value : 0xee);
        std::printf("fill of %llu bytes at %llu: %llu bytes wrong\n", (unsigned long long)f.bytes,
                    (unsigned long long)f.at, (unsigned long long)wrong);
        all = all && !wrong;
    }
    expect(all, "every fill wrote its range and nothing else");
    dev.destroyBuffer(b);
}

// The same, with no memory for a larger ring (the BoAlloc refused): written to scratch blocks that stay put while written,
// and the commit refused (ASan: no write to a block freed under it)
void overflowRing(const char *pack, const Expect &expect)
{
    gm20b::Device client;
    std::string err;
    gm20b::Buffer chain;
    if (!client.open(pack, 0, err, gm20b::Device::Role::Client) || !client.createBuffer(4096, chain)) {
        expect(false, "a client device and a buffer");
        return;
    }
    gm20b::CommandBuffer *cb = client.begin();
    if (!cb) {
        expect(false, "a command buffer");
        return;
    }
    mock_iokit_fail_next(TegraGM20BMethodBoAlloc, 1000, kIOReturnNoMemory);
    cb->use(chain, true, false);
    gm20b::BlitEncoder *e = cb->blit();
    for (uint32_t i = 0; i < 10000; i++)
        e->copyBytes(chain.va, chain.va + 4, 4);
    e->end();
    uint32_t seq = 0;
    const bool committed = cb->commit(seq);
    mock_iokit_fail_next(TegraGM20BMethodBoAlloc, 0, 0);
    if (!committed)
        cb->abandon();
    expect(!committed, "the commit of a command buffer with no ring to hold it is refused");
    const uint32_t next = copy(client, chain, chain);
    expect(next && client.wait(next), "the next command buffer runs");
    client.destroyBuffer(chain);
}

// A channel made and destroyed on another connection: the kernel's channel epoch moves
void destroyElsewhere(gm20b::Kernel &other, const Expect &expect)
{
    uint32_t channel = 0;
    const volatile uint32_t *fence = nullptr;
    const bool made = other.channel(TegraGM20BEngineCopy, channel, fence);
    expect(made, "a channel on another connection");
    if (made)
        other.closeChannel(channel);
}

void reopenRefused(gm20b::Device &dev, gm20b::Kernel &other, const Expect &expect)
{
    gm20b::Buffer a, b;
    if (!dev.createBuffer(4096, a) || !dev.createBuffer(4096, b)) {
        expect(false, "two buffers");
        return;
    }
    auto kill = [&] {
        mock_iokit_kill_next(1);
        const uint32_t killed = copy(dev, a, b);
        return killed && !dev.wait(killed);
    };
    for (const auto &[method, kr, what] :
         {std::tuple{unsigned(TegraGM20BMethodSubmitJob), int(kIOReturnNoMemory), "the context's SubmitJob"},
          std::tuple{unsigned(TegraGM20BMethodChannelCreate), int(kIOReturnNoResources), "ChannelCreate"}}) {
        expect(kill(), "a command buffer the kernel killed with its channel");
        const unsigned long creates = mock_iokit_calls(TegraGM20BMethodChannelCreate);
        mock_iokit_fail_next(method, 1, kr);
        const bool refused = !dev.begin();
        std::printf("%s refused: begin() %s\n", what, refused ? "gave none" : "gave a command buffer");
        expect(refused, "no command buffer while the kernel refuses the channel or its context");
        expect(!dev.begin() && mock_iokit_calls(TegraGM20BMethodChannelCreate) == creates + 1,
               "none asked for again while the kernel's channel epoch stays where the refusal left it");
        destroyElsewhere(other, expect);
        const uint32_t next = copy(dev, a, b);
        expect(next && dev.wait(next) && mock_iokit_calls(TegraGM20BMethodChannelCreate) == creates + 3,
               "a channel destroyed elsewhere: the next begin() makes the channel again, and its command buffer "
               "completes");
    }
    dev.destroyBuffer(a);
    dev.destroyBuffer(b);
}

void invalidateRefused(gm20b::Device &dev, const Expect &expect)
{
    gm20b::Buffer a, b;
    if (!dev.createBuffer(4096, a) || !dev.createBuffer(4096, b)) {
        expect(false, "two buffers");
        return;
    }
    const uint32_t settled = copy(dev, a, b);
    expect(settled && dev.wait(settled), "a submission after the heap's last placement");
    const uint64_t before = cap_instruction_invalidates();
    const gm20b::PackPipeline *recs = dev.pack().section<gm20b::PackPipeline>(gm20b::PackPipelines);
    for (uint32_t i = 0; i < dev.pack().header()->pipelines; i++)
        dev.pipeline(recs[i].keyHash);  // every state's programs placed
    mock_iokit_fail_next(TegraGM20BMethodSubmitJob, 1, kIOReturnNoMemory);
    expect(!copy(dev, a, b), "a submission the kernel refused");
    const uint32_t next = copy(dev, a, b);
    expect(next && dev.wait(next), "the next one");
    const uint64_t invalidates = cap_instruction_invalidates() - before;
    std::printf("instruction-cache invalidates since the placement: %llu\n", (unsigned long long)invalidates);
    expect(invalidates == 1, "the next one's prologue invalidates the instruction caches");
    dev.destroyBuffer(a);
    dev.destroyBuffer(b);
}

void waitRefused(gm20b::Device &dev, const Expect &expect)
{
    gm20b::Buffer a, b;
    if (!dev.createBuffer(4096, a) || !dev.createBuffer(4096, b)) {
        expect(false, "two buffers");
        return;
    }
    const uint32_t once = copy(dev, a, b);
    mock_iokit_fail_next(TegraGM20BMethodWaitFence, 1, kIOReturnAborted);
    const bool waited = dev.wait(once);
    expect(once && waited && dev.done(once), "a wait refused once answers when the fence has passed, and completed");
    const uint32_t stuck = copy(dev, a, b);
    mock_iokit_fail_next(TegraGM20BMethodWaitFence, 1000, kIOReturnAborted);
    const bool answered = dev.wait(stuck);
    const bool passed = dev.done(stuck);
    mock_iokit_fail_next(TegraGM20BMethodWaitFence, 0, 0);
    std::printf("a wait refused for good: %s, fence %s\n", answered ? "completed" : "failed",
                passed ? "passed" : "not passed");
    expect(stuck && (!answered || passed), "one refused for good never reads as completed before its fence");
    const uint32_t next = copy(dev, a, b);
    expect(next && dev.wait(next), "the next command buffer completes");
    dev.destroyBuffer(a);
    dev.destroyBuffer(b);
}

void channelRefused(const char *pack, gm20b::Kernel &other, const Expect &expect)
{
    gm20b::Device client;
    std::string err;
    if (!client.open(pack, 0, err, gm20b::Device::Role::Client)) {
        expect(false, "a client device");
        return;
    }
    mock_iokit_fail_next(TegraGM20BMethodChannelCreate, 1, kIOReturnNoResources);
    const bool refused = !client.begin();
    const std::string why = client.channelError();
    std::printf("refused channel: %s\n", why.c_str());
    expect(refused && why.find("ChannelCreate failed (0x") != std::string::npos,
           "no channel, and the kernel's answer kept");
    const unsigned long creates = mock_iokit_calls(TegraGM20BMethodChannelCreate);
    expect(!client.begin() && mock_iokit_calls(TegraGM20BMethodChannelCreate) == creates,
           "none asked for again at once (no kernel call a command buffer)");
    usleep(1100000);
    expect(!client.begin() && mock_iokit_calls(TegraGM20BMethodChannelCreate) == creates,
           "none asked for again a second later: the kernel's channel epoch did not move");
    destroyElsewhere(other, expect);
    gm20b::Buffer a, b;
    const bool made = client.createBuffer(4096, a) && client.createBuffer(4096, b);
    const uint32_t later = made ? copy(client, a, b) : 0;
    expect(later && client.wait(later) && mock_iokit_calls(TegraGM20BMethodChannelCreate) == creates + 2,
           "a channel destroyed elsewhere: one ask, and a command buffer that completes");
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: faultcheck <pack.bin>\n");
        return 2;
    }
    setenv("RAWCAP_LOG", "/dev/null", 0);  // the mock decodes every submission
    setenv("MOCK_GPU_US", "20000", 0);
    setenv("NVDEC_WRITETHROUGH", "1", 0);  // copies land in the memory behind them (outgrownRing reads them back)
    gm20b::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    int failures = 0;
    const Expect expect = [&](bool ok, const char *what) {
        std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    views(dev, expect);
    Pass pass;
    if (!pass.make(dev)) {
        std::fprintf(stderr, "no pack state to draw with\n");
        return 1;
    }
    instances(dev, pass, expect);
    unboundStream(dev, pass, expect);
    holeTarget(dev, pass, expect);
    barrierFlush(dev, pass, expect);
    constantAfterFill(dev, expect);
    freedHeader(dev, expect);
    partialCommit(dev, expect);
    gm20b::Kernel other;
    if (!other.open(err)) {
        std::fprintf(stderr, "a second connection: %s\n", err.c_str());
        return 1;
    }
    lostReused(other, expect);
    fills(dev, expect);
    reopenRefused(dev, other, expect);
    invalidateRefused(dev, expect);
    waitRefused(dev, expect);
    channelRefused(argv[1], other, expect);
    outgrownRing(argv[1], expect);  // last: its device's channel is the mock's latest from here on
    overflowRing(argv[1], expect);
    if (failures) {
        std::printf("FAIL: faultcheck (%d)\n", failures);
        return 1;
    }
    std::printf("PASS: faultcheck\n");
    return 0;
}
