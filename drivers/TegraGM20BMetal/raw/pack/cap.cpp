// SPDX-License-Identifier: MIT
// r7-raw capture: the mock kernel's VmBind/Exec hooks (mock_iokit.c, r5-gpuwaste's pushcount API) feeding every submission
// through nvdec (tools/nvdec.cpp) with GPU VAs resolved to the mock BOs' host memory as the CPU left it at submit time.
//   RAWCAP_LOG=<file>       canonical draw/clear/barrier records
//   RAWCAP_PROGRAMS=<file>  programs.bin (tools/mkprograms.py): identify programs exactly, compare only the cbufs they use
//   PUSHDUMP=<file>         raw "submit subc mthd data" lines (r5-gpuwaste format)
//   RAWCAP_CAPTURE=<pack.bin> RAWCAP_CAPTURE_OUT=<file>  every submission that draws decoded once more by raw::Capture
//                           (runtime/raw_capture.h) primed from the pack's context init instead of the submissions before it:
//                           one "CAPTURE <submission> <draw> ..." line per draw (test/capturecheck.py holds them to the pack)
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
#include "../runtime/raw_capture.h"
#include "../runtime/raw_pack.h"

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
std::unique_ptr<raw::Capture> gCapture;
raw::Pack gCapturePack;
FILE *gCaptureOut;
std::map<uint32_t, std::unique_ptr<nvdec::Decoder>>
    gChannels;  // r8-rawca: one decoder per channel (each has its own state)
const uint8_t *host(uint64_t va, uint64_t bytes);

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
    const char *pack = getenv("RAWCAP_CAPTURE"), *out = getenv("RAWCAP_CAPTURE_OUT");
    std::string err;
    if (pack && out && gCapturePack.load(pack, 0, err)) {
        gCapture.reset(new raw::Capture([](uint64_t va, uint64_t bytes) {
            return host(va, bytes);
        }));
        if (const char *f = getenv("RAWCAP_PROGRAMS"))
            gCapture->decoder().loadPrograms(f);
        if (gCapture->prime(gCapturePack, err))
            gCaptureOut = fopen(out, "w");
    }
    if (pack && !gCaptureOut)
        fprintf(stderr, "cap: no capture: %s\n", err.c_str());
}

void writeCapture(const std::vector<raw::CapturedState> &states)
{
    for (size_t k = 0; k < states.size(); k++) {
        const raw::CapturedState &c = states[k];
        fprintf(gCaptureOut, "CAPTURE %llu %zu vs=%s fs=%s push=%x", (unsigned long long)gSubmit, k,
                c.vertex.id.c_str(), c.pixel.id.c_str(), c.pushMask);
        for (int i = 0; i < 32; i++)
            if (c.pushMask >> i & 1)
                fprintf(gCaptureOut, " p%x=%x", 0x700 + 4 * i, c.push[i]);
        for (const auto &r : c.registers)
            fprintf(gCaptureOut, " %x=%x", r.first, r.second);
        fprintf(gCaptureOut, "\n");
    }
    fflush(gCaptureOut);
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
            if (const char *f = getenv("RAWCAP_PROGRAMS"))
                if (!d->loadPrograms(f))
                    fprintf(stderr, "cap: cannot read programs %s\n", f);
            if (const char *f = getenv("RAWCAP_LAYOUTS"))
                if (!d->loadLayouts(f))
                    fprintf(stderr, "cap: cannot read layouts %s\n", f);
        }
        dec = d.get();
    }
    const uint64_t drawsBefore = dec ? dec->draws : 0;
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
    if (gCaptureOut && dec && dec->draws > drawsBefore) {
        std::vector<std::pair<const uint32_t *, uint64_t>> segments;
        for (uint32_t i = 0; i < e->count; i++)
            if (const uint8_t *w = host(e->pushes[i].va, uint64_t(e->pushes[i].dwords) * 4))
                segments.push_back({(const uint32_t *)w, e->pushes[i].dwords});
        writeCapture(gCapture->decode(segments));
    }
}

extern "C" void pushcount_report(const char *what)
{
    std::lock_guard<std::mutex> l(gLock);
    const PushCounts &c = gC;
    const double n = c.submits ? double(c.submits) : 1.0;
    fprintf(stderr,
            "push %s: %llu submits; per submit: %.1f passes, %.1f RTs (%.1f pitch), %.1f draws, %.1f WFI, %.1f texinv, "
            "%.1f shinv, %.1f clears, %.1f CE, %.1f disp; %llu bad\n",
            what, (unsigned long long)c.submits, c.passes / n, c.targets / n, c.linearTargets / n, c.draws / n,
            c.wfi / n, c.texInval / n, c.shaderInval / n, c.clears / n, c.ceCopies / n, c.dispatches / n,
            (unsigned long long)c.unparsed);
    for (auto &c : gChannels)
        fprintf(stderr,
                "nvdec channel %u: %llu submits, %llu draws, %llu clears, %llu macro calls (%llu unknown), %llu bad "
                "headers\n",
                c.first, (unsigned long long)c.second->submits, (unsigned long long)c.second->draws,
                (unsigned long long)c.second->clears, (unsigned long long)c.second->macroCalls,
                (unsigned long long)c.second->unknownMacros, (unsigned long long)c.second->badHeaders);
    gC = PushCounts{};
}
