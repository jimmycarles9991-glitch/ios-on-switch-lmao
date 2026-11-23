// gpu.kernel.channel_epoch: the GPU's channel epoch (TegraGM20BABI.h TegraGM20BChannelEpochWord) on two TegraGM20B
// connections of the bench's own, no Metal device: a ChannelCreate refused for want of a fence slot, then a channel
// destroyed moves the epoch in both connections' fence pages before the destroy returns, and the next ChannelCreate
// and a copy-engine job on it succeed with no wait
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "TegraGM20BJobABI.h"
#include "drivers/TegraGM20BMetal/host_access.h"
#include "drivers/TegraGM20BMetal/push.h"
#include "kits/SwitchGM20B/SwitchGM20BCopy.h"
#include "apps/hwtest/bench/switch_bench.h"

namespace {

constexpr uint32_t kCopyOnly = uint32_t(TegraGM20BEngineCopy) | uint32_t(TegraGM20BEngineCopyOnly);
// a connection holds 8 fence slots (TegraGM20BUserClientInternal.h kMaxChannels): asking past twice that is no refusal
constexpr uint32_t kMostAsks = 16;
constexpr uint64_t kPage = 0x1000, kWaitNs = 2'000'000'000ull;
// LAUNCH_DMA: PIPELINED, FLUSH, pitch both sides, REMAP; SET_REMAP_COMPONENTS: DST_X..W CONST_A, component size FOUR
constexpr uint32_t kFillLaunch = 1u | 1u << 2 | 1u << 7 | 1u << 8 | 1u << 10;
constexpr uint32_t kFillWords = 0x4444u | 3u << 16;
constexpr uint8_t kFilled = 0x5a;

uint32_t epoch(const volatile uint32_t *word)
{
    return __atomic_load_n(word, __ATOMIC_ACQUIRE);
}

class EpochRun {
public:
    explicit EpochRun(switch_bench_channel_epoch &r) : r_(r)
    {}

    ~EpochRun()
    {
        for (uint32_t channel : made_)
            if (channel)
                a_.closeChannel(channel);
        for (gm20b::Bo *bo : {&push_, &target_})
            if (bo->handle)
                a_.free(*bo);
    }

    void run()
    {
        std::string err;
        if (!a_.open(err) || !b_.open(err))
            return stop("open: " + err);
        r_.epoch_flag = (a_.info().flags & TegraGM20BInfoChannelEpoch) ? 1 : 0;
        r_.fence_token = a_.info().fence_token;
        const volatile uint32_t *pageA = a_.channelEpoch(), *pageB = b_.channelEpoch();
        if (!pageA || !pageB)
            return stop("Map of GetInfo's fence_token failed");
        uint32_t channel = 0;
        int kr = KERN_SUCCESS;
        for (uint32_t ask = 0; ask < kMostAsks && kr == KERN_SUCCESS; ask++) {
            r_.epoch_before = epoch(pageA);
            if ((kr = create(channel)) == KERN_SUCCESS)
                made_.push_back(channel);
        }
        r_.slots_made = int32_t(made_.size());
        if (kr == KERN_SUCCESS || made_.empty())
            return stop(made_.empty() ? "the first ChannelCreate was refused" : "no ChannelCreate was refused");
        r_.refused_kr = kr;
        r_.epoch_refused = epoch(pageA);
        a_.closeChannel(made_[0]);
        r_.epoch_after = epoch(pageA);
        r_.epoch_other = epoch(pageB);
        made_[0] = 0;
        const uint64_t destroyed = mach_absolute_time();
        r_.recreate_kr = create(channel);
        const uint64_t created = mach_absolute_time();
        mach_timebase_info_data_t base;
        mach_timebase_info(&base);
        r_.destroy_to_create_ns = (created - destroyed) * base.numer / base.denom;
        if (r_.recreate_kr != KERN_SUCCESS)
            return stop("ChannelCreate after the destroy refused");
        made_[0] = channel;
        fillJob(channel);
    }

private:
    gm20b::Kernel a_, b_;
    switch_bench_channel_epoch &r_;
    std::vector<uint32_t> made_;
    gm20b::Bo push_, target_;

    void stop(const std::string &why)
    {
        if (!r_.why[0])
            snprintf(r_.why, sizeof r_.why, "%s", why.c_str());
    }

    // ChannelCreate's kern_return_t as the kernel answers it (gm20b::Kernel::channel keeps only its text)
    int create(uint32_t &channel)
    {
        TegraGM20BAbiChannelCreate in = {kCopyOnly, 0};
        TegraGM20BAbiChannelCreateReply reply = {};
        const int kr = a_.debugCall(TegraGM20BMethodChannelCreate, &in, sizeof in, &reply, sizeof reply);
        channel = kr == KERN_SUCCESS ? reply.channel : 0;
        return kr;
    }

    // One copy-engine job on `channel`: a page filled with kFilled, waited for, then read back under a lease
    void fillJob(uint32_t channel)
    {
        if (!a_.alloc(kPage, true, push_) || !a_.alloc(kPage, true, target_))
            return stop("BoAlloc: " + a_.error());
        auto *words = reinterpret_cast<uint32_t *>(push_.cpu);
        gm20b::Push p{words, words + push_.bytes / 4};
        p.m(NVB0B5_SET_REMAP_CONST_A, uint32_t(kFilled) * 0x01010101u, gm20b::SUBC_COPY);
        p.m(NVB0B5_SET_REMAP_COMPONENTS, kFillWords, gm20b::SUBC_COPY);
        p.inc(NVB0B5_OFFSET_OUT_UPPER, {uint32_t(target_.va >> 32), uint32_t(target_.va)}, gm20b::SUBC_COPY);
        p.inc(NVB0B5_LINE_LENGTH_IN, {uint32_t(kPage / 4), 1}, gm20b::SUBC_COPY);
        p.m(NVB0B5_LAUNCH_DMA, kFillLaunch, gm20b::SUBC_COPY);
        const TegraGM20BAbiSurfaceUse uses[] = {{push_.handle, TegraGM20BSurfaceRead, 0},
                                                {target_.handle, TegraGM20BSurfaceWrite, 0}};
        const TegraGM20BAbiPush pushed = {push_.va, uint32_t(p.at - words), 0};
        gm20b::KernelJob job;
        job.channel = channel;
        job.pushes = &pushed;
        job.pushCount = 1;
        job.uses = uses;
        job.useCount = 2;
        job.hostWritten = true;
        uint32_t seqno = 0;
        int kr = KERN_SUCCESS;
        const bool submitted = a_.submitJob(job, seqno, &kr);
        r_.job_kr = kr;
        if (!submitted)
            return stop("SubmitJob: " + a_.error());
        kr = KERN_SUCCESS;
        a_.wait(channel, seqno, kWaitNs, &kr);
        r_.wait_kr = kr;
        gm20b::HostAccess lease;
        if (!lease.acquire(a_, target_, target_.cpu, target_.bytes, false))
            return stop("CpuAcquire for a read: " + a_.error());
        uint32_t off = 0;
        for (uint64_t i = 0; i < kPage; i++)
            off += target_.cpu[i] != kFilled;
        r_.job_filled = off == 0 ? 1 : 0;
    }
};

}  // namespace

void switch_bench_channel_epoch_run(struct switch_bench_channel_epoch *r)
{
    *r = {};
    for (int32_t *each :
         {&r->epoch_flag, &r->slots_made, &r->refused_kr, &r->recreate_kr, &r->job_kr, &r->wait_kr, &r->job_filled})
        *each = -1;
    EpochRun(*r).run();
}
