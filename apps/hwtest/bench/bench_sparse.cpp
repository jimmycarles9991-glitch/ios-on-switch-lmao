// gpu.kernel.sparse_map: what MapSparse's PTEs (VALID 0, VOL 1: nouveau gm200_vmm_pgt_sparse, nvgpu
// __update_pte_sparse) give the copy engine, on a TegraGM20B connection of the bench's own, every copy one job waited for
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>
#include "TegraGM20BJobABI.h"
#include "TegraGM20BZcullABI.h"
#include "drivers/TegraGM20BMetal/host_access.h"
#include "drivers/TegraGM20BMetal/push.h"
#include "drivers/TegraGM20BMetal/zcull.h"
#include "kits/SwitchGM20B/SwitchGM20BCopy.h"
#include "apps/hwtest/bench/switch_bench.h"

namespace {

constexpr uint64_t kPage = 0x1000, kBigPage = 0x20000, kBoBytes = 0x10000;
// LAUNCH_DMA: PIPELINED, FLUSH, pitch both sides; a copy MULTI_LINE (one line), a fill REMAP
constexpr uint32_t kCopyLaunch = 1u | 1u << 2 | 1u << 7 | 1u << 8 | 1u << 9;
constexpr uint32_t kFillLaunch = 1u | 1u << 2 | 1u << 7 | 1u << 8 | 1u << 10;
// SET_REMAP_COMPONENTS: DST_X..W CONST_A, component size FOUR
constexpr uint32_t kFillWords = 0x4444u | 3u << 16;
constexpr uint64_t kWaitNs = 2'000'000'000ull;
constexpr uint8_t kS = 0xa5, kD = 0xcc, kC = 0x3c, kDropped = 0x77;

struct Use {
    uint64_t va, bytes;
    bool write, hostWritten;
};

class SparseRun {
public:
    explicit SparseRun(switch_bench_sparse &r) : r_(r)
    {}

    ~SparseRun()
    {
        for (uint32_t channel : {main_, throwaway_})
            if (channel)
                k_.closeChannel(channel);
        for (gm20b::Bo *bo : {&push_, &s_, &d_, &c_})
            if (bo->handle)
                k_.free(*bo);
    }

    void run()
    {
        std::string err;
        if (!k_.open(err))
            return stop("open: " + err);
        r_.info_bit = k_.sparse() ? 1 : 0;
        if (!r_.info_bit)
            return stop("GetInfo answers no TegraGM20BInfoSparse");
        if (!setUp())
            return;
        if (headAndTail() && bigPages() && besideBigPage() && writesDropped() && replacements() && badArguments())
            r_.faults = mainFaulted() ? 1 : 0;
        if (r_.faults == 0)
            unmappedRead();
    }

private:
    gm20b::Kernel k_;
    switch_bench_sparse &r_;
    gm20b::Bo push_, s_, d_, c_;
    uint32_t main_ = 0, throwaway_ = 0;
    uint64_t v_ = 0, x_ = 0;

    void stop(const std::string &why)
    {
        if (!r_.why[0])
            snprintf(r_.why, sizeof r_.why, "%s", why.c_str());
    }

    bool stopped(const std::string &why)
    {
        stop(why);
        return false;
    }

    int vmBind(uint32_t op, uint64_t va, uint64_t range, uint64_t handle = 0)
    {
        TegraGM20BAbiVmBind b = {};
        b.count = 1;
        b.ops[0] = {op, 0, handle, 0, va, range};
        return k_.debugCall(TegraGM20BMethodVmBind, &b, offsetof(TegraGM20BAbiVmBind, ops) + sizeof b.ops[0], nullptr,
                            0);
    }

    bool bind(uint32_t op, uint64_t va, uint64_t range, uint64_t handle = 0)
    {
        const int kr = vmBind(op, va, range, handle);
        char line[96];
        snprintf(line, sizeof line, "VmBind op %u of %#llx+%#llx: 0x%x", op, (unsigned long long)va,
                 (unsigned long long)range, unsigned(kr));
        return kr == KERN_SUCCESS || stopped(line);
    }

    // Reserved VA aligned to a big page, `bytes` of it (the reservation is never given back: the connection closes)
    uint64_t reserve(uint64_t bytes)
    {
        const uint64_t base = k_.reserveVa(bytes + kBigPage);
        return base ? (base + kBigPage - 1) & ~(kBigPage - 1) : 0;
    }

    bool setUp()
    {
        if (!k_.alloc(kPage * 16, true, push_) || !k_.alloc(kBoBytes, true, s_) || !k_.alloc(kBoBytes, true, d_))
            return stopped("BoAlloc: " + k_.error());
        const volatile uint32_t *fence = nullptr;
        if (!k_.channel(TegraGM20BEngineCopy, main_, fence))
            return stopped("ChannelCreate: " + k_.error());
        v_ = reserve(0x80000);
        x_ = reserve(0x40000);
        if (!v_ || !x_)
            return stopped("no GPU VA left to reserve");
        TegraGM20BAbiBoAlloc a = {kBigPage, 17, TegraGM20BBoContiguous};
        TegraGM20BAbiBoAllocReply reply = {};
        if (const int kr = k_.debugCall(TegraGM20BMethodBoAlloc, &a, sizeof a, &reply, sizeof reply);
            kr != KERN_SUCCESS)
            return stopped("BoAlloc of a contiguous big page: 0x" + hex(kr));
        c_.handle = reply.handle;
        c_.bytes = kBigPage;
        return cpuFill(s_, kS) && bind(TegraGM20BBindMap, v_, kBoBytes, s_.handle);
    }

    static std::string hex(int kr)
    {
        char text[16];
        snprintf(text, sizeof text, "%x", unsigned(kr));
        return text;
    }

    bool cpuFill(const gm20b::Bo &bo, uint8_t value)
    {
        gm20b::HostAccess lease;
        if (!lease.acquire(k_, bo, bo.cpu, bo.bytes, true))
            return stopped("CpuAcquire for a write: " + k_.error());
        memset(bo.cpu, value, bo.bytes);
        return true;
    }

    // [offset, offset + bytes) of `bo` all `value`, read under a lease; -1: the lease was refused
    int cpuHolds(const gm20b::Bo &bo, uint64_t offset, uint64_t bytes, uint8_t value)
    {
        gm20b::HostAccess lease;
        if (!lease.acquire(k_, bo, bo.cpu, bo.bytes, false)) {
            stop("CpuAcquire for a read: " + k_.error());
            return -1;
        }
        for (uint64_t i = offset; i < offset + bytes; i++)
            if (bo.cpu[i] != value)
                return 0;
        return 1;
    }

    static void copy(gm20b::Push &p, uint64_t from, uint64_t to, uint64_t bytes)
    {
        p.inc(NVB0B5_OFFSET_IN_UPPER, {uint32_t(from >> 32), uint32_t(from), uint32_t(to >> 32), uint32_t(to)},
              gm20b::SUBC_COPY);
        p.inc(NVB0B5_LINE_LENGTH_IN, {uint32_t(bytes), 1}, gm20b::SUBC_COPY);
        p.m(NVB0B5_LAUNCH_DMA, kCopyLaunch, gm20b::SUBC_COPY);
    }

    static void fill(gm20b::Push &p, uint64_t at, uint64_t bytes, uint8_t value)
    {
        p.m(NVB0B5_SET_REMAP_CONST_A, uint32_t(value) * 0x01010101u, gm20b::SUBC_COPY);
        p.m(NVB0B5_SET_REMAP_COMPONENTS, kFillWords, gm20b::SUBC_COPY);
        p.inc(NVB0B5_OFFSET_OUT_UPPER, {uint32_t(at >> 32), uint32_t(at)}, gm20b::SUBC_COPY);
        p.inc(NVB0B5_LINE_LENGTH_IN, {uint32_t(bytes / 4), 1}, gm20b::SUBC_COPY);
        p.m(NVB0B5_LAUNCH_DMA, kFillLaunch, gm20b::SUBC_COPY);
    }

    // One job of `emit`'s methods on `channel`, waited for. kr: WaitFence's (kIOReturnNotResponding: killed)
    bool job(uint32_t channel, const std::function<void(gm20b::Push &)> &emit, const std::vector<Use> &uses, int &kr)
    {
        auto *words = reinterpret_cast<uint32_t *>(push_.cpu);
        gm20b::Push p{words, words + push_.bytes / 4};
        emit(p);
        std::vector<TegraGM20BAbiSurfaceUse> named = {{push_.handle, TegraGM20BSurfaceRead, 0}};
        for (const Use &u : uses)
            named.push_back({u.va,
                             uint32_t(TegraGM20BSurfaceGpuAddress) | uint32_t(TegraGM20BSurfaceRange) |
                                 (u.write ? uint32_t(TegraGM20BSurfaceWrite) : uint32_t(TegraGM20BSurfaceRead)) |
                                 (u.hostWritten ? uint32_t(TegraGM20BSurfaceHostWritten) : 0u),
                             uint32_t(u.bytes)});
        const TegraGM20BAbiPush pushed = {push_.va, uint32_t(p.at - words), 0};
        gm20b::KernelJob j;
        j.channel = channel;
        j.pushes = &pushed;
        j.pushCount = 1;
        j.uses = named.data();
        j.useCount = uint32_t(named.size());
        j.hostWritten = true;
        uint32_t seqno = 0;
        if (!k_.submitJob(j, seqno, &kr))
            return stopped("SubmitJob: 0x" + hex(kr) + " " + k_.error());
        return k_.wait(channel, seqno, kWaitNs, &kr);
    }

    Use sUse() const
    {
        return {s_.va, s_.bytes, false, true};
    }

    Use cUse(bool write) const
    {
        return {x_, kBigPage, write, false};
    }

    // `emit`'s job on the main channel, W (D) refilled with kD first
    bool intoW(const std::function<void(gm20b::Push &)> &emit, std::vector<Use> uses)
    {
        int kr = 0;
        if (!cpuFill(d_, kD))
            return false;
        uses.push_back({d_.va, d_.bytes, true, true});
        return job(main_, emit, uses, kr) || stopped("a job writing W did not complete: 0x" + hex(kr));
    }

    // D's [0, split) all `head` and [split, end) all `tail`: 1, else 0; -1 a lease was refused
    int wHolds(uint64_t split, uint8_t head, uint64_t end, uint8_t tail)
    {
        const int first = cpuHolds(d_, 0, split, head), second = cpuHolds(d_, split, end - split, tail);
        return first < 0 || second < 0 ? -1 : first && second;
    }

    bool headAndTail()
    {
        if (!bind(TegraGM20BBindMapSparse, v_ + kBoBytes, kBoBytes) || !intoW(
                                                                           [&](gm20b::Push &p) {
                                                                               copy(p, v_ + 0xf000, d_.va, 0x2000);
                                                                           },
                                                                           {sUse()}))
            return false;
        r_.head_bytes_kept = cpuHolds(d_, 0, kPage, kS);
        r_.tail_read_zero = cpuHolds(d_, kPage, kPage, 0);
        return r_.head_bytes_kept >= 0 && r_.tail_read_zero >= 0;
    }

    bool bigPages()
    {
        if (!bind(TegraGM20BBindMapSparse, v_ + 0x40000, 0x40000) || !intoW(
                                                                         [&](gm20b::Push &p) {
                                                                             copy(p, v_ + 0x5f000, d_.va, kPage);
                                                                         },
                                                                         {}))
            return false;
        r_.big_read_zero = cpuHolds(d_, 0, kPage, 0);
        return r_.big_read_zero >= 0;
    }

    bool besideBigPage()
    {
        int kr = 0;
        if (!bind(TegraGM20BBindMap, x_, kBigPage, c_.handle) || !bind(TegraGM20BBindMapSparse, x_ + kBigPage, 0x4000))
            return false;
        if (!job(
                main_,
                [&](gm20b::Push &p) {
                    fill(p, x_, kBigPage, kC);
                },
                {cUse(true)}, kr))
            return stopped("the fill of C did not complete: 0x" + hex(kr));
        if (!intoW(
                [&](gm20b::Push &p) {
                    copy(p, x_ + kBigPage - kPage, d_.va, 0x2000);
                },
                {cUse(false)}))
            return false;
        r_.beside_big_read_zero = wHolds(kPage, kC, 0x2000, 0);
        return r_.beside_big_read_zero >= 0;
    }

    // The dropped fill and its read, then every byte of S (CPU) and C (copied out half by half) as it was
    bool writesDropped()
    {
        if (!intoW(
                [&](gm20b::Push &p) {
                    fill(p, v_ + kBoBytes, kPage, kDropped);
                    copy(p, v_ + kBoBytes, d_.va, kPage);
                },
                {}))
            return false;
        const int w = wHolds(kPage, 0, kBoBytes, kD), sKept = cpuHolds(s_, 0, kBoBytes, kS);
        int cKept = 1;
        for (uint64_t half = 0; half < kBigPage && cKept == 1; half += kBoBytes) {
            if (!intoW(
                    [&](gm20b::Push &p) {
                        copy(p, x_ + half, d_.va, kBoBytes);
                    },
                    {cUse(false)}))
                return false;
            cKept = cpuHolds(d_, 0, kBoBytes, kC);
        }
        if (w < 0 || sKept < 0 || cKept < 0)
            return false;
        r_.write_dropped = w && sKept && cKept;
        return true;
    }

    bool replacements()
    {
        const auto readV = [&](gm20b::Push &p) {
            copy(p, v_, d_.va, kPage);
        };
        if (!bind(TegraGM20BBindMapSparse, v_, kBoBytes) || !intoW(readV, {sUse()}))
            return false;
        r_.replaces_map = cpuHolds(d_, 0, kPage, 0);
        if (r_.replaces_map < 0 || !bind(TegraGM20BBindMap, v_, kBoBytes, s_.handle) || !intoW(readV, {sUse()}))
            return false;
        r_.map_replaces = cpuHolds(d_, 0, kPage, kS);
        return r_.map_replaces >= 0;
    }

    bool badArguments()
    {
        const uint64_t end = k_.info().va_end;
        r_.bad_args_kr[0] = vmBind(TegraGM20BBindMapSparse, v_ + 0x20800, kPage);
        r_.bad_args_kr[1] = vmBind(TegraGM20BBindMapSparse, end - kPage, 2 * kPage);
        r_.bad_args_refused =
            r_.bad_args_kr[0] == kIOReturnBadArgument && r_.bad_args_kr[1] == kIOReturnBadArgument ? 1 : 0;
        return true;
    }

    bool mainFaulted()
    {
        TegraGM20BAbiStatus st = {};
        return k_.lost(main_) || !k_.status(main_, st) || st.error != 0;
    }

    void unmappedRead()
    {
        const volatile uint32_t *fence = nullptr;
        if (!k_.channel(TegraGM20BEngineCopy, throwaway_, fence))
            return stop("ChannelCreate of the throwaway channel: " + k_.error());
        if (!bind(TegraGM20BBindUnmap, v_ + kBoBytes, kBoBytes))
            return;
        r_.unmapped_va = v_ + kBoBytes;
        int kr = 0;
        const bool completed = job(
            throwaway_,
            [&](gm20b::Push &p) {
                copy(p, r_.unmapped_va, d_.va, kPage);
            },
            {{d_.va, d_.bytes, true, false}}, kr);
        TegraGM20BAbiStatus st = {};
        if (!k_.status(throwaway_, st))
            return stop("GetStatus of the throwaway channel refused");
        r_.fault_error = st.error;
        r_.fault_va = st.fault_va;
        r_.unmapped_read_faults = !completed && kr == kIOReturnNotResponding && st.error == 1 &&
                                          st.fault_va >= r_.unmapped_va && st.fault_va < r_.unmapped_va + kBoBytes
                                      ? 1
                                      : 0;
    }
};

}  // namespace

void switch_bench_sparse_map(struct switch_bench_sparse *r)
{
    *r = {};
    for (int32_t *each :
         {&r->head_bytes_kept, &r->tail_read_zero, &r->big_read_zero, &r->beside_big_read_zero, &r->write_dropped,
          &r->replaces_map, &r->map_replaces, &r->bad_args_refused, &r->faults, &r->unmapped_read_faults})
        *each = -1;
    SparseRun(*r).run();
}

uint32_t switch_bench_kernel_flags(void)
{
    gm20b::Kernel k;
    std::string err;
    return k.open(err) ? k.info().flags : 0;
}

int32_t switch_bench_zcull_info(uint32_t *flags, uint32_t info[12], uint32_t zc[12])
{
    gm20b::Kernel k;
    std::string err;
    if (!k.open(err))
        return -1;
    *flags = k.info().flags;
    memcpy(info, k.info().zcull, sizeof k.info().zcull);
    TegraGM20BAbiZcullInfo layout = {};
    const int kr = k.zcullInfo(layout);
    memcpy(zc, &layout, sizeof layout);
    return kr;
}
