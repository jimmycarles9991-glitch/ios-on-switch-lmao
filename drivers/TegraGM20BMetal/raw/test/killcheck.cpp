// SPDX-License-Identifier: MIT
// raw GM20B path: a channel kill (the mock's mock_iokit_kill_next, as the kext's client fault isolation) fails exactly the
// submissions it took, as the kill record names them: the one in flight and the one it came with fail; one that completed
// before it does not, even when its wait comes after the channel was made again; the next command buffer completes on a new
// channel, and memory written before the kill keeps its bytes. An MMU fault's VA reaches the device, which names the memory
// that holds it.
// usage: killcheck <pack.bin>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../runtime/raw_gpu.h"

extern "C" void mock_iokit_kill_next(uint32_t error);
extern "C" void mock_iokit_kill_next_at(uint32_t error, uint64_t fault_va);
extern "C" unsigned long mock_iokit_calls(unsigned method);

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: killcheck <pack.bin>\n");
        return 2;
    }
    setenv("MOCK_GPU_US", "20000", 0);
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    int failures = 0;
    auto expect = [&](bool ok, const char *what) {
        std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    constexpr uint32_t kBytes = 4096;
    raw::Buffer a, b;
    if (!dev.createBuffer(kBytes, a) || !dev.createBuffer(kBytes, b)) {
        std::fprintf(stderr, "no buffers\n");
        return 1;
    }
    memset(a.cpu, 0x5a, kBytes);
    auto copy = [&](const raw::Buffer &from, const raw::Buffer &to) {
        uint32_t seqno = 0;
        raw::CommandBuffer *cb = dev.begin();
        raw::BlitEncoder *e = cb ? cb->blit() : nullptr;
        if (!e)
            return 0u;
        e->copyBytes(from.va, to.va, kBytes);
        e->end();
        return cb->commit(seqno) ? seqno : 0u;
    };

    const uint32_t before = copy(a, b);
    usleep(100000);
    const uint32_t running = copy(a, b);
    mock_iokit_kill_next_at(1, b.va + 0x40);
    const uint32_t faulting = copy(b, a);
    expect(before && running && faulting, "the three command buffers were submitted");
    const unsigned long creates = mock_iokit_calls(TegraGM20BMethodChannelCreate);
    const uint32_t after = copy(a, b);
    expect(mock_iokit_calls(TegraGM20BMethodChannelCreate) == creates + 1, "the next command buffer got a new channel");
    expect(dev.wait(before), "one that completed before the kill did not fail, waited for after the new channel");
    expect(!dev.wait(running), "the one in flight failed");
    expect(!dev.wait(faulting), "the one the kill came with failed");
    expect(after && dev.wait(after), "the next command buffer completed");
    expect(dev.lostError() == 1 && dev.lostFaultVa() == b.va + 0x40,
           "the kill's error and fault VA reached the device");
    const std::string owner = dev.vaOwner(b.va + 0x40);
    std::printf("fault VA owner: %s\n", owner.c_str());
    expect(owner.find("chunk +0x") != std::string::npos, "the device names the memory the fault VA is in");
    expect(dev.vaOwner(1) == "none of the raw device's memory", "a VA outside its memory is none of its own");
    bool kept = true;
    for (uint32_t i = 0; i < kBytes; i++)
        kept = kept && a.cpu[i] == 0x5a;
    expect(kept, "memory written before the kill keeps its bytes");
    dev.destroyBuffer(a);
    dev.destroyBuffer(b);
    if (failures) {
        std::printf("FAIL: killcheck (%d)\n", failures);
        return 1;
    }
    std::printf("PASS: killcheck\n");
    return 0;
}
