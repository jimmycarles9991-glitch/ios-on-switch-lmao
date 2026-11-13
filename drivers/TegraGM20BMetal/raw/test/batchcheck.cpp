// SPDX-License-Identifier: MIT
// Actual raw encoder over the host command decoder. Checks emission, batching and compatibility invalidation; the
// synthetic shaders deliberately do not establish rendered pixels or hardware preemption latency.
#include "../runtime/raw_gpu.h"
#include "../runtime/raw_compute.h"
#include "TegraGM20BJobABI.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <set>
#include <unistd.h>
#include "wrapcheck.h"
#include "rangecheck.h"
#include "retrycheck.h"

extern "C" unsigned long mock_iokit_calls(unsigned method);
extern "C" uint64_t cap_draws(void);
extern "C" void mock_iokit_fail_after(unsigned method, unsigned skip, unsigned count, int kr);
extern "C" bool cap_read(uint64_t va, void *out, uint64_t bytes);

static std::vector<uint64_t> launchAddresses()
{
    fflush(nullptr);
    std::ifstream dump(getenv("PUSHDUMP"));
    std::vector<uint64_t> addresses;
    std::string line;
    while (std::getline(dump, line)) {
        std::istringstream fields(line);
        uint64_t submit;
        uint32_t subchannel, method, value;
        if (fields >> submit >> subchannel >> std::hex >> method >> value)
            if (subchannel == 1 && method == 0x2b4)
                addresses.push_back(uint64_t(value) << 8);
    }
    return addresses;
}

static uint64_t cbufAddress(const uint32_t qmd[64])
{
    return qmd[29] | (uint64_t(qmd[30] & 0xff) << 32);
}

int main()
{
    setenv("RAWCAP_LOG", "/private/tmp/switch-batchcheck-capture.log", 0);
    setenv("PUSHDUMP", "/private/tmp/switch-batchcheck-push.log", 0);
    char path[] = "/private/tmp/switch-batch-pack-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0)
        return 2;
    raw::PackHeader header{};
    memcpy(header.magic, "GM20BPK1", 8);
    header.version = raw::kPackVersion;
    for (auto &section : header.section)
        section.offset = sizeof header;
    const raw::PackInitMethod init[] = {{0, 0, 0xb197, 0}, {1, 0, 0xb1c0, 0}, {4, 0, 0xb0b5, 0}};
    header.section[raw::PackInit].bytes = sizeof init;
    const bool written =
        write(fd, &header, sizeof header) == sizeof header && write(fd, init, sizeof init) == sizeof init;
    close(fd);
    raw::Device device;
    std::string error;
    const bool opened = written && device.open(path, 0, error);
    unlink(path);
    if (!opened) {
        fprintf(stderr, "device: %s\n", error.c_str());
        return 1;
    }
    unsigned failures = 0, checks = 0;
    auto expect = [&](bool ok, const char *label) {
        ++checks;
        failures += !ok;
        printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    };
    raw::Texture image;
    if (!device.createTexture(16, 16, 80, image, true))
        return 2;
    raw::PackPipeline record{};
    record.vertexSlot = 0xff;
    raw::Pipeline pipeline;
    pipeline.rec = &record;
    pipeline.colorFormats[0] = 80;
    raw::PassDesc pass;
    pass.count = 1;
    pass.color[0].texture = &image;
    pass.color[0].load = raw::ColorAttachment::Load;
    pass.color[0].store = true;
    const char *why = nullptr;
    auto begin = [&]() {
        auto *cb = device.begin();
        auto *encoder = cb ? cb->render(pass) : nullptr;
        if (encoder) {
            encoder->setPipeline(&pipeline);
            encoder->setScissor(0, 0, 1, 1);
        }
        return std::make_pair(cb, encoder);
    };
    auto repeated = [&](unsigned count, unsigned batches) {
        auto [cb, encoder] = begin();
        if (!encoder) {
            fprintf(stderr, "repeated begin: %s\n", device.channelError().c_str());
            return false;
        }
        const uint64_t before = cap_draws();
        const unsigned long submits = mock_iokit_calls(TegraGM20BMethodExec);
        for (unsigned i = 0; i < count; ++i) {
            encoder->setScissor(i & 1, 0, 1, 1);
            if (!encoder->draw(3, 0, 3, 1, 0, &why)) {
                fprintf(stderr, "repeated draw: %s\n", why ? why : "unknown");
                cb->abandon();
                return false;
            }
        }
        encoder->end();
        uint32_t sequence = 0;
        const unsigned long waits = mock_iokit_calls(TegraGM20BMethodWaitFence);
        if (!cb->commit(sequence)) {
            fprintf(stderr, "repeated commit: %s\n", device.channelError().c_str());
            cb->abandon();
            return false;
        }
        const bool noWaits = mock_iokit_calls(TegraGM20BMethodWaitFence) == waits;
        const bool finished = device.wait(sequence);
        const bool ok = noWaits && finished && cap_draws() - before == count &&
                        mock_iokit_calls(TegraGM20BMethodExec) - submits == batches;
        if (!ok)
            fprintf(stderr, "repeated result: noWaits=%d finished=%d draws=%llu/%u submits=%lu/%u error=%s\n", noWaits,
                    finished, (unsigned long long)(cap_draws() - before), count,
                    mock_iokit_calls(TegraGM20BMethodExec) - submits, batches, device.channelError().c_str());
        return ok;
    };
    expect(repeated(1, 1), "one small draw stays one submission");
    expect(repeated(1000, 8), "1000 draws survive eight bounded submissions");
    expect(repeated(256, 2), "exact boundary includes final stores without an extra submission");
    pipeline.fragmentInstructions = raw::BatchWork::kLimit;
    expect(repeated(3, 1), "the render server keeps a client's preemption-sized draws in one batch");
    pipeline.fragmentInstructions = raw::BatchWork::kRenderServerLimit;
    expect(repeated(3, 3), "expensive draws each expose a legal preemption boundary");
    pipeline.fragmentInstructions = 1;

    {
        auto [cb, encoder] = begin();
        expect(encoder && encoder->draw(3, 0, 3, 1, 0, &why), "compatible pass prepares successfully");
        raw::DepthStencilDesc desc;
        desc.depthWrite = true;
        const raw::DepthStencilState depth(desc);
        encoder->setDepthStencil(&depth);
        expect(!encoder->draw(3, 0, 3, 1, 0, &why) && cb->refusal.id == raw::RefDepthStencil,
               "changed depth state invalidates cached compatibility");
        cb->abandon();
    }
    {
        auto [cb, encoder] = begin();
        expect(encoder && encoder->draw(3, 0, 3, 1, 0, &why), "sample check starts from compatible pipeline");
        raw::Pipeline multisample = pipeline;
        multisample.samples = 2;
        encoder->setPipeline(&multisample);
        expect(!encoder->draw(3, 0, 3, 1, 0, &why) && cb->refusal.id == raw::RefSamples,
               "sample count mismatch is refused after a prior compatible draw");
        cb->abandon();
    }
    {
        auto [cb, encoder] = begin();
        expect(encoder && encoder->draw(3, 0, 3, 1, 0, &why), "next encoder resets prior depth state");
        raw::Pipeline different = pipeline;
        different.colorFormats[0] = 70;
        encoder->setPipeline(&different);
        expect(!encoder->draw(3, 0, 3, 1, 0, &why) && cb->refusal.id == raw::RefColorFormats,
               "changed pipeline invalidates cached compatibility");
        cb->abandon();
    }
    {
        raw::Texture other = image;
        other.format = 70;
        pass.color[0].texture = &other;
        auto [cb, encoder] = begin();
        expect(encoder && !encoder->draw(3, 0, 3, 1, 0, &why) && cb->refusal.id == raw::RefColorFormats,
               "new pass cannot reuse previous attachment compatibility");
        cb->abandon();
        pass.color[0].texture = &image;
    }
    {
        raw::Buffer arguments;
        if (!device.createBuffer(256, arguments))
            return 2;
        const uint32_t words[] = {3, 2, 0, 0};
        memcpy(arguments.cpu, words, sizeof words);
        auto [cb, encoder] = begin();
        const uint64_t before = cap_draws();
        const unsigned long submits = mock_iokit_calls(TegraGM20BMethodExec);
        const bool draws =
            encoder && encoder->drawIndirect(3, &arguments, 0, &why) && encoder->draw(3, 0, 3, 1, 0, &why);
        if (encoder)
            encoder->end();
        uint32_t sequence = 0;
        expect(draws && cb->commit(sequence) && device.wait(sequence) && cap_draws() - before == 3 &&
                   mock_iokit_calls(TegraGM20BMethodExec) - submits == 2,
               "indirect arguments stay with their macro and direct work resumes after the cut");
        device.destroyBuffer(arguments);
    }
    {
        raw::PackKernel kernel{};
        kernel.regionWord = -1;
        kernel.qmd[18] = 1u << 16;
        kernel.qmd[19] = 1u | (1u << 16);
        kernel.qmd[20] = 1;
        kernel.entries = 1;
        kernel.readOnlyEntries = 1;
        kernel.setBytes = 16;
        kernel.entry[0].kind = raw::KernelBuffer;
        kernel.entry[0].window = 32;
        raw::ComputeState state;
        state.rec = &kernel;
        state.cbufType[0] = raw::CbufRoot;
        raw::Buffer input;
        if (!device.createBuffer(256, input))
            return 2;
        auto *cb = device.begin();
        raw::ComputeEncoder encoder(*cb, device);
        encoder.setState(&state);
        const uint32_t one[] = {1, 1, 1};
        bool dispatched = true;
        const uint64_t uploaded = device.stats.uploadBytes;
        for (unsigned i = 0; i < 1000; ++i) {
            encoder.setState(&state);
            encoder.setBuffer(0, &input, 0);
            encoder.setTexture(0, nullptr);
            encoder.setSampler(0, nullptr);
            dispatched &= encoder.dispatch(one, one, &why);
        }
        expect(device.stats.uploadBytes - uploaded == 256 + 2048 + 1000 * 256,
               "unchanged dispatches upload one descriptor/root snapshot and fresh QMDs only");
        encoder.end();
        const unsigned long submits = mock_iokit_calls(TegraGM20BMethodExec);
        uint32_t sequence = 0;
        expect(dispatched && cb->commit(sequence) && device.wait(sequence) &&
                   mock_iokit_calls(TegraGM20BMethodExec) - submits == 8,
               "1000 dispatch launches form eight bounded submissions");
        std::ifstream trace(getenv("RAWCAP_LOG"));
        std::string line;
        unsigned launches = 0;
        while (std::getline(trace, line))
            launches += line.rfind("DISPATCH grid=1,1,1 block=1,1,1 ", 0) == 0;
        expect(launches == 1000, "decoder observes every dispatch and its complete launch dimensions");
        const auto addresses = launchAddresses();
        bool immutable = addresses.size() == 1000;
        std::set<uint64_t> roots;
        for (uint64_t address : addresses) {
            uint32_t qmd[64], root[512], descriptor[4];
            immutable &= cap_read(address, qmd, sizeof qmd);
            const uint64_t rootVa = cbufAddress(qmd);
            roots.insert(rootVa);
            immutable &= cap_read(rootVa, root, sizeof root);
            const uint64_t setVa = root[14] | uint64_t(root[15]) << 32;
            immutable &= cap_read(setVa, descriptor, sizeof descriptor);
            immutable &= root[3] == 1 && root[4] == 1 && root[5] == 1 && root[16] == 16 &&
                         (descriptor[0] | uint64_t(descriptor[1]) << 32) == input.va && descriptor[2] == 32;
        }
        expect(immutable && roots.size() == 1 && std::set<uint64_t>(addresses.begin(), addresses.end()).size() == 1000,
               "all GPU QMDs retain distinct launch storage and the exact immutable descriptor/root bytes");
        device.destroyBuffer(input);
    }
    {
        raw::Buffer input;
        raw::Texture secondImage;
        raw::Sampler samplers[2];
        raw::SamplerDesc samplerDesc;
        if (!device.createBuffer(256, input) || !device.createTexture(16, 16, 80, secondImage, true) ||
            !device.createSampler(samplerDesc, samplers[0]))
            return 2;
        samplerDesc.minFilter = samplerDesc.magFilter = 1;
        if (!device.createSampler(samplerDesc, samplers[1]))
            return 2;
        raw::PackKernel kernels[3]{};
        auto &kernel = kernels[0];
        kernel.regionWord = 8;
        kernel.qmd[18] = 1u << 16;
        kernel.qmd[19] = 1u | (1u << 16);
        kernel.qmd[20] = 1;
        kernel.entries = 3;
        kernel.setBytes = 32;
        kernel.readOnlyEntries = 7;
        kernel.entry[0].kind = raw::KernelBuffer;
        kernel.entry[0].window = 32;
        kernel.entry[1].kind = raw::KernelTexture;
        kernel.entry[1].offset = 16;
        kernel.entry[2].kind = raw::KernelSampler;
        kernel.entry[2].offset = 16;
        kernel.pushMask = 1;
        kernel.pushWords[0] = 0x11;
        kernels[1] = kernels[2] = kernel;
        kernels[1].pushWords[0] = 0x22;
        kernels[2].entry[1].window = 0x1b;  // deliberately incompatible two-byte view of BGRA8
        raw::ComputeState states[3];
        for (unsigned i = 0; i < 3; ++i) {
            states[i].rec = &kernels[i];
            states[i].cbufType[0] = raw::CbufRoot;
        }

        struct Expected {
            uint64_t buffer;
            uint32_t grid, threads, push, handle;
        };

        std::vector<Expected> expected;
        auto *cb = device.begin();
        raw::ComputeEncoder encoder(*cb, device);
        bool dispatched = true;
        auto launch = [&](unsigned state, uint32_t grid, uint32_t threads, uint32_t offset, unsigned texture,
                          unsigned sampler) {
            const auto *t = texture ? &secondImage : &image;
            encoder.setState(&states[state]);
            encoder.setBuffer(0, &input, offset);
            encoder.setTexture(0, t);
            encoder.setSampler(0, &samplers[sampler]);
            const uint32_t groups[] = {grid, 1, 1}, size[] = {threads, 1, 1};
            dispatched &= encoder.dispatch(groups, size, &why);
            expected.push_back(
                {input.va + offset, grid, threads, kernels[state].pushWords[0], t->tic | samplers[sampler].tsc << 20});
        };
        launch(0, 1, 1, 0, 0, 0);
        launch(0, 1, 1, 0, 0, 0);
        launch(0, 2, 1, 0, 0, 0);
        launch(0, 2, 1, 16, 0, 0);
        launch(0, 2, 1, 16, 0, 1);
        launch(0, 2, 1, 16, 1, 1);
        launch(1, 2, 1, 16, 1, 1);
        launch(1, 2, 2, 16, 1, 1);
        encoder.setState(&states[2]);
        const uint32_t one[] = {1, 1, 1};
        expect(!encoder.dispatch(one, one, &why), "incompatible descriptor rebuild refuses before a launch");
        launch(1, 2, 2, 16, 1, 1);
        encoder.end();
        uint32_t sequence = 0;
        expect(dispatched && cb->commit(sequence) && device.wait(sequence),
               "changed bindings, grid, push constants and recovery all submit");
        const auto addresses = launchAddresses();
        bool matches = addresses.size() == 1000 + expected.size();
        std::vector<uint64_t> roots, sets;
        for (size_t i = 0; i < expected.size() && matches; ++i) {
            uint32_t qmd[64]{}, root[512]{}, descriptor[8]{};
            matches &= cap_read(addresses[1000 + i], qmd, sizeof qmd);
            roots.push_back(cbufAddress(qmd));
            matches &= cap_read(roots.back(), root, sizeof root);
            sets.push_back(root[14] | uint64_t(root[15]) << 32);
            matches &= cap_read(sets.back(), descriptor, sizeof descriptor);
            const auto &e = expected[i];
            matches &= root[3] == e.grid && root[448] == e.push && root[459] == e.threads - 1 &&
                       root[460] == e.grid * e.threads && descriptor[2] == 32 && descriptor[4] == e.handle &&
                       (descriptor[0] | uint64_t(descriptor[1]) << 32) == e.buffer;
        }
        expect(matches, "every recorded launch retains its exact earlier resource and dispatch snapshot");
        expect(matches && roots[0] == roots[1] && roots[1] != roots[2] && sets[1] == sets[2] && roots[6] != roots[7] &&
                   sets[6] == sets[7] && roots[7] != roots[8],
               "only changed root/descriptor inputs allocate; failed rebuild cannot revive a stale snapshot");
        device.destroyBuffer(input);
        device.destroyTexture(secondImage);
        for (auto &sampler : samplers)
            device.destroySampler(sampler);
    }
    {
        auto [cb, encoder] = begin();
        const uint64_t before = cap_draws();
        for (unsigned i = 0; i < 300; ++i)
            if (!encoder->draw(3, 0, 3, 1, 0, &why))
                return 2;
        encoder->end();
        mock_iokit_fail_after(TegraGM20BMethodExec, 1, 1, kIOReturnNoMemory);
        uint32_t sequence = 0;
        expect(!cb->commit(sequence) && cb->partial && cap_draws() - before == raw::BatchWork::kOperations,
               "later submission failure drains the accepted prefix and reports partial execution");
        cb->abandon();
        expect(repeated(1, 1), "encoding recovers on a fresh channel after partial submission failure");
    }
    if (getenv("MOCK_NATIVE_JOBS")) {
        auto [cb, encoder] = begin();
        const uint64_t before = cap_draws();
        for (unsigned i = 0; i < 256; ++i)
            if (!encoder->draw(3, 0, 3, 1, 0, &why))
                return 2;
        encoder->end();
        mock_iokit_fail_after(TegraGM20BMethodSubmitJob, 0, 1, kIOReturnCannotLock);
        uint32_t sequence = 0;
        expect(!cb->commit(sequence) && cb->admissionBlocked && !cb->partial && cap_draws() == before,
               "CPU conflict refuses before any draw publishes");
        cb->abandon();
        expect(repeated(1, 1), "abandoned unsent range leaves the channel usable by another queue");
        expect(repeated(256, 2), "retry emits the complete previously unsent range once");
    }
    raw::DeviceHarness::computeRetry(device, expect);
    raw::DeviceHarness::ranges(device, expect);
    raw::DeviceHarness::wrap(device, pipeline, expect);
    device.destroyTexture(image);
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
