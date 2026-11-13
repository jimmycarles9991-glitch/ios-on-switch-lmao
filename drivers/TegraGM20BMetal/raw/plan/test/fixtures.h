// SPDX-License-Identifier: MIT
#pragma once
#include "../plan_build.h"
#include <string>
#include <vector>

namespace raw::plan::test {

// Builds a Recording the way the runtime would record one: resources by first use, one encoder at a time
class Rec {
public:
    Recording r;

    uint32_t texture(uint16_t format, uint32_t width, uint32_t height, uint32_t flags = 0, uint16_t levels = 1,
                     uint8_t samples = 1, uint32_t memory = kNone);
    uint32_t buffer(uint64_t bytes, uint32_t flags = ResourceFacts::HostVisible, uint32_t memory = kNone);
    uint32_t sampler(uint32_t flags = 0);
    uint32_t program(const ProgramFacts &p);
    uint32_t state(uint32_t program, const std::vector<Binding> &bindings);

    void render(const PassDesc &pass);
    void blit();
    void compute(bool serial = true);
    void end();

    void draw(uint32_t state, uint32_t count = 6, uint8_t indexSize = 2, uint32_t indices = kNone);
    void drawIndirect(uint32_t state, uint32_t arguments);
    void control(const Control &c);
    void transfer(const Transfer &t);
    void dispatch(uint32_t program, const std::vector<Binding> &bindings, uint32_t shadowSlots = 0);
    void present(uint32_t drawable);
    void signal(uint32_t event, uint64_t value);
    void wait(uint32_t event, uint64_t value);

private:
    uint32_t nextMemory_ = 1;
    void op(OpKind kind, uint32_t payload);
};

Binding texture(uint32_t slot, uint8_t stage = 1, uint8_t index = 0);
Binding constants(uint32_t slot, uint8_t stage = 1, uint8_t index = 0);
Binding vertices(uint32_t slot, uint8_t index = 0);
Binding storage(uint32_t slot, bool write, uint8_t stage = 2, uint8_t index = 0);

Attachment colour(uint32_t texture, Load load, Store store, uint16_t level = 0);
PassDesc pass(std::vector<Attachment> colours, uint32_t width, uint32_t height);

// A Core Animation state over its colour formats: writes them all, fetches `fetched` at `rung` (interlock: storage form)
ProgramFacts caState(uint64_t key, std::vector<uint16_t> formats, uint8_t fetched = 0, Rung rung = Rung::None,
                     uint8_t blendReads = 0);

// The pack's own states for every format but depth (resolve) and the 32-bit float ones (mip)
Caps chipCaps(uint32_t proven = 0);

// Core Animation's glass frames as backboardd records them with tile shaders off (Apple3): the first frame of a glass
// layer (content, quarter-resolution capture, three pyramid levels, glass) and a steady one (the backdrop kept)
Recording glassFrame0();
Recording glassFrame1();

// A scroll frame shaped as pacing.list's measured one (12 passes, ~92 draws, ~27 targets); the pass composition is assumed.
// Its offscreen passes' memoryless attachments share one memory as raw::Device::backing's views of a tag do, or each has
// its own
Recording scrollFrame(bool aliasedScratch = true);

// Every kind of node at once: an event wait, a copy into a texture and a fill, a dispatch writing what a pass reads as
// constants, a pass with a resolve, an extended-range stand-in and a refreshed shadow, a mip chain, an indirect draw, the
// present and a signal
Recording mixedFrame();

// Each node and run with the barriers placed before it and the nodes its edges lead from, one line each
std::string describe(const WorkGraph &g);

}  // namespace raw::plan::test
