// SPDX-License-Identifier: MIT
// r7-raw capture: the mock kernel's VmBind/Exec hooks (mock_iokit.c, r5-gpuwaste's pushcount API) feeding every submission
// through nvdec (tools/nvdec.cpp) with GPU VAs resolved to the mock BOs' host memory as the CPU left it at submit time.
//   RAWCAP_LOG=<file>       canonical draw/clear/barrier records
//   RAWCAP_PROGRAMS=<file>  programs.bin (tools/mkprograms.py): identify programs exactly, compare only the cbufs they use
//   PUSHDUMP=<file>         raw "submit subc mthd data" lines (r5-gpuwaste format)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include "TegraGM20BABI.h"
#include "nvdec.h"

namespace {
#include "parse.h"

struct Bind {
    uint64_t range;
    int fd;
    void *import;
    uint64_t offset, bytes;
    uint8_t *base;
};

std::map<uint64_t, Bind> gBinds;
std::map<int, void *> gMaps;
std::mutex gLock;
PushCounts gC;
FILE *gDump, *gLog;
uint64_t gSubmit;
bool gDecode;  // RAWCAP_LOG or NVDEC_WRITETHROUGH given
std::map<uint32_t, std::unique_ptr<nvdec::Decoder>>
    gChannels;  // r8-rawca: one decoder per channel (each has its own state)
uint32_t gLastChannel;
const uint8_t *host(uint64_t va, uint64_t bytes);

extern "C" uint32_t mock_iokit_va_kind(uint64_t va);  // mock_comp.h

extern "C" void cap_describe(uint64_t va)
{
    auto it = gBinds.upper_bound(va);
    if (it == gBinds.begin()) {
        fprintf(stderr, "va %llx: no bind\n", (unsigned long long)va);
        return;
    }
    --it;
    const Bind &b = it->second;
    const uint8_t *h = host(va, 16);
    fprintf(stderr, "va %llx: bind va %llx range %llx fd %d import %p off %llx bytes %llx host %p first %08x\n",
            (unsigned long long)va, (unsigned long long)it->first, (unsigned long long)b.range, b.fd, b.import,
            (unsigned long long)b.offset, (unsigned long long)b.bytes, h, h ? *(const uint32_t *)h : 0);
}

const uint8_t *host(uint64_t va, uint64_t bytes)
{
    auto it = gBinds.upper_bound(va);
    if (it == gBinds.begin())
        return nullptr;
    --it;
    const Bind &b = it->second;
    if (va + bytes > it->first + b.range)
        return nullptr;
    // (mapped at bind time: the mock closes a freed BO's fd and the next BO may get the same number)
    uint8_t *base = b.import ? (uint8_t *)b.import : b.base;
    if (!base)
        return nullptr;
    return base + b.offset + (va - it->first);
}

void dumpPush(const uint32_t *w, uint64_t n)
{
    uint32_t op = 0, subc = 0, mthd = 0, left = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (left) {
            fprintf(gDump, "%llu %u %04x %08x\n", (unsigned long long)gSubmit, subc, mthd, w[i]);
            if (op == 1)
                mthd += 4;
            else if (op == 5) {
                op = 3;
                mthd += 4;
            }
            left--;
            continue;
        }
        const uint32_t h = w[i];
        if (!h)
            continue;
        op = h >> 29;
        const uint32_t count = (h >> 16) & 0x1fff;
        subc = (h >> 13) & 7;
        mthd = (h & 0xfff) << 2;
        if (op == 4)
            fprintf(gDump, "%llu %u %04x %08x\n", (unsigned long long)gSubmit, subc, mthd, count);
        else if (op == 1 || op == 3 || op == 5)
            left = count;
        else {
            fprintf(gDump, "%llu bad %08x\n", (unsigned long long)gSubmit, h);
            return;
        }
    }
}

void init()
{
    static bool done;
    if (done)
        return;
    done = true;
    if (const char *f = getenv("PUSHDUMP"))
        gDump = fopen(f, "w");
    if (const char *f = getenv("RAWCAP_LOG"))
        gLog = fopen(f, "w");
    // NVDEC_WRITETHROUGH asks for the GPU's copies in host memory, which only the decoder makes: it runs then too, its
    // trace going nowhere when no log was asked for
    if (!gLog && getenv("NVDEC_WRITETHROUGH"))
        gLog = fopen("/dev/null", "w");
    gDecode = gLog != nullptr;
}

}

// A host test that reads the GPU's results back (NVDEC_WRITETHROUGH) needs NVK's copies and fills on the copy engine, which
// the decoder runs: NVK's default way, compute dispatches and 3D draws, runs shaders this mock does not. Both ways write the
// same bytes (Vulkan's copy semantics); the raw pack's probe never sets NVDEC_WRITETHROUGH and keeps the device's way.
__attribute__((constructor)) static void copiesOnCopyEngine()
{
    if (getenv("NVDEC_WRITETHROUGH"))
        setenv("NVK_COPIES", "ce", 0);
}

// The GPU's side of NVDEC_SCRUB (nvdec.cpp): zero [va, va + bytes) in the mock BO behind it.
extern "C" void cap_scrub(uint64_t va, uint64_t bytes)
{
    const uint8_t *h = host(va, bytes);
    if (h)
        memset((void *)h, 0, bytes);
}

extern "C" void pushcount_bind(uint64_t va, uint64_t range, int fd, void *import, uint64_t offset, uint64_t bytes)
{
    std::lock_guard<std::mutex> l(gLock);
    for (auto it = gBinds.begin(); it != gBinds.end();) {
        if (it->first < va + range && va < it->first + it->second.range)
            it = gBinds.erase(it);
        else
            ++it;
    }
    uint8_t *base = nullptr;
    if (fd >= 0 && !import) {
        void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        base = p == MAP_FAILED ? nullptr : (uint8_t *)p;
    }
    if (fd >= 0 || import)
        gBinds[va] = Bind{range, fd, import, offset, bytes, base};
}

extern "C" void pushcount_exec(const struct TegraGM20BAbiExec *e)
{
    std::lock_guard<std::mutex> l(gLock);
    if (!e->count)
        return;
    static const bool fast = getenv("MOCK_FAST") != nullptr;  // benchmarks: the kernel's work is not the encoder's
    if (fast)
        return;
    init();
    gSubmit++;
    gC.submits++;
    gLastChannel = e->channel;
    PushParse p = {};
    nvdec::Decoder *dec = nullptr;
    if (gDecode) {
        auto &d = gChannels[e->channel];
        if (!d) {
            d.reset(new nvdec::Decoder(
                [](uint64_t va, uint64_t bytes) {
                    return host(va, bytes);
                },
                gLog));
            d->scrub = cap_scrub;
            d->describe = cap_describe;
            d->vaKind = mock_iokit_va_kind;
            if (const char *f = getenv("RAWCAP_PROGRAMS"))
                if (!d->loadPrograms(f))
                    fprintf(stderr, "cap: cannot read programs %s\n", f);
            if (const char *f = getenv("RAWCAP_LAYOUTS"))
                if (!d->loadLayouts(f))
                    fprintf(stderr, "cap: cannot read layouts %s\n", f);
        }
        dec = d.get();
    }
    if (dec)
        dec->submitBegin();
    for (uint32_t i = 0; i < e->count; i++) {
        const uint8_t *w = host(e->pushes[i].va, uint64_t(e->pushes[i].dwords) * 4);
        gC.bytes += uint64_t(e->pushes[i].dwords) * 4;
        if (!w) {
            p.left = 0;
            continue;
        }
        gC.readBytes += uint64_t(e->pushes[i].dwords) * 4;
        pushParse(gC, p, (const uint32_t *)w, e->pushes[i].dwords);
        if (gDump)
            dumpPush((const uint32_t *)w, e->pushes[i].dwords);
        if (dec)
            dec->push((const uint32_t *)w, e->pushes[i].dwords);
    }
    if (dec) {
        dec->submitEnd();
        fflush(gLog);
    }
}

// What reached the GPU, for the raw harnesses' checks: the draws of every channel so far and a 3D method's value after the last
// submission (decoding only: RAWCAP_LOG or NVDEC_WRITETHROUGH set), the instruction-cache invalidates submitted
extern "C" uint64_t cap_draws(void)
{
    std::lock_guard<std::mutex> l(gLock);
    uint64_t n = 0;
    for (auto &c : gChannels)
        n += c.second->draws;
    return n;
}

extern "C" uint64_t cap_instruction_invalidates(void)
{
    std::lock_guard<std::mutex> l(gLock);
    return gC.instructionInval;
}

extern "C" uint64_t
cap_shader_invalidates(void)  // INVALIDATE_SHADER_CACHES (flush and invalidate, with a wait for idle)
{
    std::lock_guard<std::mutex> l(gLock);
    return gC.shaderInval;
}

extern "C" uint64_t cap_constant_invalidates(void)
{
    std::lock_guard<std::mutex> l(gLock);
    return gC.constantInval;
}

// Draws of every channel so far that read a constant buffer the copy engine wrote with no constant-cache invalidate after
extern "C" uint64_t cap_stale_constant_reads(void)
{
    std::lock_guard<std::mutex> l(gLock);
    uint64_t n = 0;
    for (auto &c : gChannels)
        n += c.second->staleConstantReads;
    return n;
}

// `bytes` at GPU VA `va` as the GPU reads them; false where they are not in one bound range
extern "C" bool cap_read(uint64_t va, void *out, uint64_t bytes)
{
    std::lock_guard<std::mutex> l(gLock);
    const uint8_t *h = host(va, bytes);
    if (h)
        memcpy(out, h, bytes);
    return h != nullptr;
}

// The constant buffer the last channel's decoder holds at `slot` of binding group `group` (0 vertex, 4 pixel); false: none
extern "C" bool cap_cbuf(int group, int slot, uint64_t *va, uint32_t *bytes)
{
    std::lock_guard<std::mutex> l(gLock);
    const auto it = gChannels.find(gLastChannel);
    return it != gChannels.end() && it->second->bound(group, slot, *va, *bytes);
}

extern "C" uint32_t cap_reg3d(uint32_t mthd)
{
    std::lock_guard<std::mutex> l(gLock);
    const auto it = gChannels.find(gLastChannel);
    return it == gChannels.end() ? 0 : it->second->reg3d(mthd);
}

extern "C" void pushcount_report(const char *what)
{
    std::lock_guard<std::mutex> l(gLock);
    const PushCounts &c = gC;
    const double n = c.submits ? double(c.submits) : 1.0;
    fprintf(stderr,
            "push %s: %llu submits; per submit: %.1f passes, %.1f RTs (%.1f pitch), %.1f draws, %.1f WFI, %.1f texinv, "
            "%.1f shinv, %.1f cbinv, %.1f clears, %.1f CE, %.1f disp; %llu bad\n",
            what, (unsigned long long)c.submits, c.passes / n, c.targets / n, c.linearTargets / n, c.draws / n,
            c.wfi / n, c.texInval / n, c.shaderInval / n, c.constantInval / n, c.clears / n, c.ceCopies / n,
            c.dispatches / n, (unsigned long long)c.unparsed);
    for (auto &c : gChannels)
        fprintf(stderr,
                "nvdec channel %u: %llu submits, %llu draws, %llu clears, %llu macro calls (%llu unknown), %llu bad "
                "headers, %llu stale constant reads\n",
                c.first, (unsigned long long)c.second->submits, (unsigned long long)c.second->draws,
                (unsigned long long)c.second->clears, (unsigned long long)c.second->macroCalls,
                (unsigned long long)c.second->unknownMacros, (unsigned long long)c.second->badHeaders,
                (unsigned long long)c.second->staleConstantReads);
    gC = PushCounts{};
}
