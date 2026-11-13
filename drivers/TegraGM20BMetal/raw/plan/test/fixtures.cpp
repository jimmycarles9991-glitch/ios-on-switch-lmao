// SPDX-License-Identifier: MIT
#include "fixtures.h"
#include <cstdio>

namespace raw::plan::test {

uint32_t Rec::texture(uint16_t format, uint32_t width, uint32_t height, uint32_t flags, uint16_t levels,
                      uint8_t samples, uint32_t memory)
{
    ResourceFacts f;
    f.kind = ResourceFacts::Texture;
    f.handle = {uint32_t(r.resources.size()), 1};
    f.memory = memory != kNone ? memory : nextMemory_++;
    f.flags = flags;
    f.format = format;
    f.width = width;
    f.height = height;
    f.levels = levels;
    f.samples = samples;
    f.layout = flags & ResourceFacts::Drawable ? Layout::Pitch : Layout::BlockLinear;
    r.resources.push_back(f);
    return uint32_t(r.resources.size() - 1);
}

uint32_t Rec::buffer(uint64_t bytes, uint32_t flags, uint32_t memory)
{
    ResourceFacts f;
    f.kind = ResourceFacts::Buffer;
    f.handle = {uint32_t(r.resources.size()), 1};
    f.memory = memory != kNone ? memory : nextMemory_++;
    f.bytes = bytes;
    f.flags = flags;
    r.resources.push_back(f);
    return uint32_t(r.resources.size() - 1);
}

uint32_t Rec::sampler(uint32_t flags)
{
    ResourceFacts f;
    f.kind = ResourceFacts::Sampler;
    f.flags = flags;
    r.resources.push_back(f);
    return uint32_t(r.resources.size() - 1);
}

uint32_t Rec::program(const ProgramFacts &p)
{
    r.programs.push_back(p);
    return uint32_t(r.programs.size() - 1);
}

uint32_t Rec::state(uint32_t program, const std::vector<Binding> &bindings)
{
    DrawState s;
    s.program = program;
    s.bindings = {uint32_t(r.bindings.size()), uint32_t(bindings.size())};
    r.bindings.insert(r.bindings.end(), bindings.begin(), bindings.end());
    r.states.push_back(s);
    return uint32_t(r.states.size() - 1);
}

void Rec::render(const PassDesc &p)
{
    r.passes.push_back(p);
    Encoder e;
    e.kind = EncoderKind::Render;
    e.pass = uint32_t(r.passes.size() - 1);
    e.ops.first = uint32_t(r.ops.size());
    r.encoders.push_back(e);
}

void Rec::blit()
{
    Encoder e;
    e.kind = EncoderKind::Blit;
    e.ops.first = uint32_t(r.ops.size());
    r.encoders.push_back(e);
}

void Rec::compute(bool serial)
{
    Encoder e;
    e.kind = EncoderKind::Compute;
    e.serial = serial;
    e.ops.first = uint32_t(r.ops.size());
    r.encoders.push_back(e);
}

void Rec::end()
{
    Encoder &e = r.encoders.back();
    e.ops.count = uint32_t(r.ops.size()) - e.ops.first;
}

void Rec::op(OpKind kind, uint32_t payload)
{
    r.ops.push_back({kind, payload});
}

void Rec::draw(uint32_t state, uint32_t count, uint8_t indexSize, uint32_t indices)
{
    Draw d;
    d.state = state;
    d.primitive = 3;
    d.count = count;
    d.indexSize = indexSize;
    d.indices = indices;
    r.draws.push_back(d);
    op(OpKind::Draw, uint32_t(r.draws.size() - 1));
}

void Rec::drawIndirect(uint32_t state, uint32_t arguments)
{
    Draw d;
    d.state = state;
    d.indirect = true;
    d.arguments = arguments;
    r.draws.push_back(d);
    op(OpKind::Draw, uint32_t(r.draws.size() - 1));
}

void Rec::control(const Control &c)
{
    r.controls.push_back(c);
    op(OpKind::Control, uint32_t(r.controls.size() - 1));
}

void Rec::transfer(const Transfer &t)
{
    r.transfers.push_back(t);
    op(OpKind::Transfer, uint32_t(r.transfers.size() - 1));
}

void Rec::dispatch(uint32_t program, const std::vector<Binding> &bindings, uint32_t shadowSlots)
{
    Dispatch d;
    d.program = program;
    d.bindings = {uint32_t(r.bindings.size()), uint32_t(bindings.size())};
    d.grid = {64, 64, 1};
    d.threads = {8, 8, 1};
    d.imageShadowSlots = shadowSlots;
    r.bindings.insert(r.bindings.end(), bindings.begin(), bindings.end());
    r.dispatches.push_back(d);
    op(OpKind::Dispatch, uint32_t(r.dispatches.size() - 1));
}

void Rec::present(uint32_t drawable)
{
    r.boundaries.push_back({Boundary::Present, uint32_t(r.encoders.size()), drawable});
}

void Rec::signal(uint32_t event, uint64_t value)
{
    r.events.push_back({event, value, true, false});
    r.boundaries.push_back({Boundary::Signal, uint32_t(r.encoders.size()), uint32_t(r.events.size() - 1)});
}

void Rec::wait(uint32_t event, uint64_t value)
{
    r.events.push_back({event, value, false, false});
    r.boundaries.push_back({Boundary::Wait, uint32_t(r.encoders.size()), uint32_t(r.events.size() - 1)});
}

Binding texture(uint32_t slot, uint8_t stage, uint8_t index)
{
    return {slot, stage, index, Path::Texture, false, 0};
}

Binding constants(uint32_t slot, uint8_t stage, uint8_t index)
{
    return {slot, stage, index, Path::Constant, false, 0};
}

Binding vertices(uint32_t slot, uint8_t index)
{
    return {slot, 0, index, Path::Vertex, false, 0};
}

Binding storage(uint32_t slot, bool write, uint8_t stage, uint8_t index)
{
    return {slot, stage, index, Path::Storage, write, 0};
}

Attachment colour(uint32_t texture, Load load, Store store, uint16_t level)
{
    Attachment a;
    a.texture = texture;
    a.load = load;
    a.store = store;
    a.level = level;
    return a;
}

PassDesc pass(std::vector<Attachment> colours, uint32_t width, uint32_t height)
{
    PassDesc p;
    for (size_t i = 0; i < colours.size() && i < 8; ++i)
        p.color[i] = colours[i];
    p.width = width;
    p.height = height;
    return p;
}

ProgramFacts caState(uint64_t key, std::vector<uint16_t> formats, uint8_t fetched, Rung rung, uint8_t blendReads)
{
    ProgramFacts p;
    p.key = key;
    for (size_t i = 0; i < formats.size(); ++i) {
        p.colorFormats[i] = formats[i];
        p.writeMask |= uint8_t(1u << i);
    }
    p.readMask = blendReads;
    p.fetchMask = fetched;
    for (uint32_t c = 0; c < 8; ++c)
        if (fetched >> c & 1)
            p.fetchRung[c] = uint8_t(rung);
    if (rung == Rung::Interlock) {
        p.storageMask = fetched;
        p.storageWriteMask = fetched & p.writeMask;
        p.forms |= ProgramFacts::StorageForm;
    }
    if (rung == Rung::Barrier)
        p.forms |= ProgramFacts::BarrierForm;
    return p;
}

Caps chipCaps(uint32_t proven)
{
    Caps caps;
    caps.proven = proven;
    caps.own.has = [](const void *, OwnState s, uint16_t format) {
        if (s == OwnState::Resolve)
            return format != 252 && format != 260;
        if (s == OwnState::Mip)
            return format != 55 && format != 125;
        return true;
    };
    return caps;
}

namespace {

// The glass frame's pipelines by Core Animation name, their keys made up
struct Glass {
    Rec rec;
    uint32_t drawable, c1, c2, vb, ub, ib;
    uint32_t content, contentState;

    Glass()
    {
        drawable =
            rec.texture(80, 1280, 720, ResourceFacts::Drawable | ResourceFacts::Surface | ResourceFacts::Rendered);
        c1 = rec.texture(115, 1280, 720, ResourceFacts::Memoryless | ResourceFacts::Compressed, 1, 1, 100);
        c2 = rec.texture(55, 1280, 720, ResourceFacts::Memoryless | ResourceFacts::Compressed, 1, 1, 101);
        vb = rec.buffer(1 << 16);
        ub = rec.buffer(1 << 12);
        ib = rec.buffer(1 << 12);
        content = rec.program(caState(0xa3f0, {80, 115, 55}));
        contentState = rec.state(content, {vertices(vb), constants(ub, 0), constants(ub, 1)});
    }

    PassDesc drawablePass() const
    {
        return pass({colour(drawable, Load::Load, Store::Store), colour(c1, Load::DontCare, Store::DontCare),
                     colour(c2, Load::DontCare, Store::DontCare)},
                    1280, 720);
    }

    void contentDraws()
    {
        for (int i = 0; i < 21; ++i)
            rec.draw(contentState, 6, 2, ib);
    }

    void glassDraws(uint32_t pyramid)
    {
        const uint32_t background = rec.program(caState(0x76a5, {80, 115, 55}, 0, Rung::None, 1));
        const uint32_t merge = rec.program(caState(0x6e46, {80, 115, 55}, 7, Rung::Interlock));
        const uint32_t one = rec.texture(80, 1, 1), white = rec.texture(80, 1, 1), noise = rec.texture(80, 32, 32);
        const uint32_t s0 = rec.state(background, {vertices(vb), constants(ub), texture(pyramid, 1, 3)});
        const uint32_t s1 = rec.state(
            merge, {vertices(vb), constants(ub), texture(one, 1, 3), texture(white, 1, 12), texture(noise, 1, 9)});
        rec.draw(s0, 24, 0);
        rec.draw(s0, 96, 2, ib);
        rec.draw(s0, 30, 0);
        rec.draw(s1, 6, 0);
        rec.draw(s1, 48, 2, ib);
    }
};

}  // namespace

Recording glassFrame0()
{
    Glass g;
    Rec &rec = g.rec;
    const uint32_t capture = rec.texture(80, 320, 128, ResourceFacts::Rendered);
    const uint32_t capture1 =
        rec.texture(115, 320, 128, ResourceFacts::Memoryless | ResourceFacts::Compressed, 1, 1, 100);
    const uint32_t pyramid = rec.texture(80, 384, 128, ResourceFacts::Rendered, 3);
    const uint32_t pyramid1 =
        rec.texture(115, 384, 128, ResourceFacts::Memoryless | ResourceFacts::Compressed, 1, 1, 100);
    rec.render(g.drawablePass());
    g.contentDraws();
    rec.end();
    const uint32_t ds4 = rec.program(caState(0x7d54, {80, 115}));
    rec.render(pass({colour(capture, Load::Clear, Store::Store), colour(capture1, Load::DontCare, Store::DontCare)},
                    320, 124));
    const uint32_t s = rec.state(ds4, {vertices(g.vb), constants(g.ub), texture(g.drawable, 1, 3)});
    rec.draw(s, 48, 2, g.ib);
    rec.draw(s, 18, 2, g.ib);
    rec.end();
    const uint32_t img = rec.program(caState(0x1309, {80, 115}));
    rec.render(pass({colour(pyramid, Load::Clear, Store::Store), colour(pyramid1, Load::DontCare, Store::DontCare)},
                    384, 128));
    rec.draw(rec.state(img, {vertices(g.vb), constants(g.ub), texture(capture, 1, 3)}), 6, 2, g.ib);
    rec.end();
    const uint32_t vrd = rec.program(caState(0x7a7d, {80, 115}));
    const uint32_t vs = rec.state(vrd, {vertices(g.vb), constants(g.ub), texture(pyramid, 1, 3)});
    for (uint16_t level = 1; level < 3; ++level) {
        rec.render(
            pass({colour(pyramid, Load::Clear, Store::Store, level), colour(pyramid1, Load::DontCare, Store::DontCare)},
                 384u >> level, 128u >> level));
        rec.draw(vs, 6, 2, g.ib);
        rec.end();
    }
    rec.render(g.drawablePass());
    g.glassDraws(pyramid);
    rec.end();
    rec.present(g.drawable);
    return rec.r;
}

Recording glassFrame1()
{
    Glass g;
    Rec &rec = g.rec;
    const uint32_t pyramid = rec.texture(80, 384, 128, ResourceFacts::Rendered, 3);
    rec.render(g.drawablePass());
    g.contentDraws();
    g.glassDraws(pyramid);
    rec.end();
    rec.present(g.drawable);
    return rec.r;
}

Recording scrollFrame(bool aliasedScratch)
{
    Rec rec;
    const uint32_t drawable =
        rec.texture(80, 1280, 720, ResourceFacts::Drawable | ResourceFacts::Surface | ResourceFacts::Rendered);
    const uint32_t c1 = rec.texture(115, 1280, 720, ResourceFacts::Memoryless | ResourceFacts::Compressed, 1, 1, 100);
    const uint32_t c2 = rec.texture(55, 1280, 720, ResourceFacts::Memoryless | ResourceFacts::Compressed, 1, 1, 101);
    const uint32_t vb = rec.buffer(1 << 16), ub = rec.buffer(1 << 12), ib = rec.buffer(1 << 12);
    const uint32_t atlas = rec.texture(10, 1024, 1024);
    const uint32_t layerState = rec.program(caState(0x5c01, {80, 115}));
    const uint32_t screenState = rec.program(caState(0x5c02, {80, 115, 55}, 0, Rung::None, 1));
    const auto offscreen = [&](uint32_t count, uint32_t sampled) {
        const uint32_t layer = rec.texture(80, 640, 160, ResourceFacts::Rendered);
        const uint32_t scratch = rec.texture(115, 640, 160, ResourceFacts::Memoryless | ResourceFacts::Compressed, 1, 1,
                                             aliasedScratch ? 100 : kNone);
        rec.render(pass({colour(layer, Load::Clear, Store::Store), colour(scratch, Load::DontCare, Store::DontCare)},
                        640, 160));
        const uint32_t s = rec.state(layerState, {vertices(vb), constants(ub), texture(sampled, 1, 0)});
        for (uint32_t i = 0; i < count; ++i)
            rec.draw(s, 6, 2, ib);
        rec.end();
        return layer;
    };
    const auto onscreen = [&](uint32_t count, const std::vector<uint32_t> &layers) {
        rec.render(pass({colour(drawable, Load::Load, Store::Store), colour(c1, Load::DontCare, Store::DontCare),
                         colour(c2, Load::DontCare, Store::DontCare)},
                        1280, 720));
        for (uint32_t i = 0; i < count; ++i)
            rec.draw(rec.state(screenState, {vertices(vb), constants(ub), texture(layers[i % layers.size()], 1, 0)}), 6,
                     2, ib);
        rec.end();
    };
    std::vector<uint32_t> first, second;
    for (int i = 0; i < 4; ++i)
        first.push_back(offscreen(4, atlas));
    onscreen(30, first);
    for (int i = 0; i < 4; ++i)
        second.push_back(offscreen(3, atlas));
    onscreen(20, second);
    const uint32_t pocket = rec.texture(80, 320, 32, ResourceFacts::Rendered);
    const uint32_t pocket1 =
        rec.texture(115, 320, 32, ResourceFacts::Memoryless | ResourceFacts::Compressed, 1, 1, 100);
    rec.render(
        pass({colour(pocket, Load::Clear, Store::Store), colour(pocket1, Load::DontCare, Store::DontCare)}, 320, 32));
    rec.draw(rec.state(layerState, {vertices(vb), constants(ub), texture(drawable, 1, 0)}), 6, 2, ib);
    rec.draw(rec.state(layerState, {vertices(vb), constants(ub), texture(drawable, 1, 0)}), 6, 2, ib);
    rec.end();
    onscreen(8, {pocket});
    rec.present(drawable);
    return rec.r;
}

Recording mixedFrame()
{
    Rec rec;
    const uint32_t event = rec.sampler();
    const uint32_t staging = rec.buffer(1 << 16), args = rec.buffer(256), uniforms = rec.buffer(4096);
    const uint32_t atlas = rec.texture(10, 256, 256);
    const uint32_t standIn = rec.texture(115, 64, 64), xr = rec.texture(552, 64, 64, ResourceFacts::ExtendedRange);
    const uint32_t pool = rec.texture(10, 360, 240), thumb = rec.texture(10, 360, 240, ResourceFacts::SampledShadow);
    const uint32_t ms = rec.texture(80, 256, 256, ResourceFacts::Memoryless, 1, 4);
    const uint32_t resolved = rec.texture(80, 256, 256, ResourceFacts::MipChain2D, 9);
    const uint32_t drawable =
        rec.texture(80, 1280, 720, ResourceFacts::Drawable | ResourceFacts::Surface | ResourceFacts::Rendered);
    rec.r.resources[xr].standIn = standIn;
    rec.r.resources[thumb].shadow = pool;
    rec.wait(event, 1);
    rec.blit();
    Transfer upload;
    upload.kind = Transfer::CopyBufferToTexture;
    upload.source = staging;
    upload.destination = atlas;
    upload.bytes = 4096;
    rec.transfer(upload);
    Transfer fill;
    fill.kind = Transfer::FillBuffer;
    fill.destination = args;
    fill.bytes = 64;
    rec.transfer(fill);
    rec.end();
    rec.compute();
    ProgramFacts kernel;
    kernel.compute = true;
    rec.dispatch(rec.program(kernel), {storage(uniforms, true), storage(args, true, 2, 1)});
    rec.end();
    Attachment a = colour(ms, Load::Clear, Store::Resolve);
    a.resolve = resolved;
    rec.render(pass({a}, 256, 256));
    ProgramFacts p = caState(7, {80});
    p.samples = 4;
    rec.draw(rec.state(rec.program(p), {vertices(staging), constants(uniforms), texture(atlas), texture(xr, 1, 1),
                                        texture(thumb, 1, 2)}));
    rec.end();
    rec.blit();
    Transfer mips;
    mips.kind = Transfer::Mipmaps;
    mips.destination = resolved;
    rec.transfer(mips);
    rec.end();
    rec.render(pass({colour(drawable, Load::Load, Store::Store)}, 1280, 720));
    rec.drawIndirect(rec.state(rec.program(caState(8, {80})), {texture(resolved)}), args);
    rec.end();
    rec.present(drawable);
    rec.signal(event, 2);
    return rec.r;
}

std::string describe(const WorkGraph &g)
{
    static const char *const bits[] = {"tiled", "wfi", "flush", "data",   "const", "instr", "tex",
                                       "ps",    "tic", "tsc",   "setref", "ce_np", "switch"};
    const auto names = [](Barriers b) {
        std::string out;
        for (uint32_t i = 0; i < 13; ++i)
            if (b >> i & 1)
                out += std::string(out.empty() ? "" : "+") + bits[i];
        return out.empty() ? std::string("-") : out;
    };
    std::string out = "  entry " + names(g.entry) + "\n";
    for (uint32_t i = 0; i < g.nodes.size(); ++i) {
        const Node &n = g.nodes[i];
        char line[256];
        std::snprintf(line, sizeof line, "  %2u %-10s %-20s%s before %-24s after", i, kNodeKindNames[size_t(n.kind)],
                      kRuleNames[size_t(n.rule)], n.continues ? " (fused)" : "", names(n.before).c_str());
        out += line;
        for (uint32_t e = 0; e < n.after.count; ++e)
            out += " " + std::to_string(g.edges[n.after.first + e]);
        out += "\n";
        for (uint32_t k = 0; k < n.runs.count; ++k) {
            const DrawRun &run = g.runs[n.runs.first + k];
            if (run.before)
                out += "       run " + std::to_string(k) + " before " + names(run.before) + "\n";
        }
    }
    return out + "  exit " + names(g.exit) + "\n";
}

}  // namespace raw::plan::test
