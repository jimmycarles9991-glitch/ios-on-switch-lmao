// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVJPG.cpp on the host against a model of the engine: a partition, its clamps, NVJPG's clock and reset, a
// falcon that answers only once started on a powered, clocked, released engine, and host1x channel 3 whose OP_DONE
// increments the model's syncpoint when the falcon answers. The idle gate's thread call is fired by hand.
#include "switch_nvjpg_gate_stubs.h"
#include <algorithm>
#include <map>
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../TegraNVJPG.cpp"

// A falcon v1 image: magic, version 1, 0x200 bytes, the OS header at 0x100 naming 0x80 bytes of code and of data.
__asm__(".section __DATA,__const\n"
        ".p2align 4\n"
        ".globl _tegra_nvjpg_fw_start\n"
        "_tegra_nvjpg_fw_start:\n"
        ".long 0x10de, 1, 0x200, 0x100, 0, 0x200, 0, 0\n"
        ".space 0xe0\n"
        ".long 0, 0x80, 0x80, 0x80, 13\n"
        ".space 0xec\n"
        ".globl _tegra_nvjpg_fw_end\n"
        "_tegra_nvjpg_fw_end:\n"
        ".text\n");

namespace {
struct Engine {
    bool partitionOn = true, clamped = false, clockOn = false, inReset = true, falconRunning = false;
    bool flushed = false, nvdecPartitionOn = false;
    std::vector<std::string> events;
    bool stallDecodes = false, deadAfterReset = false;
    int boots = 0, partitionPowerOns = 0, teardowns = 0, restarts = 0;
    uint32_t syncpt = 369427729;
    std::vector<std::vector<uint32_t>> streams;
    std::map<unsigned, bool> clocks, resets;

    bool reachable() const
    {
        return partitionOn && !clamped && clockOn && !inReset && !flushed;
    }

    // The steps since `from` that name one of `of`, in order.
    std::vector<std::string> steps(size_t from, std::initializer_list<const char *> of) const
    {
        std::vector<std::string> out;
        for (size_t i = from; i < events.size(); i++)
            for (const char *e : of)
                if (events[i] == e)
                    out.push_back(events[i]);
        return out;
    }
};

Engine gEngine;
std::vector<uint8_t> gNvjpgRegisters(0x40000), gMcRegisters(0x1000);
uint64_t gNextPa = 0x80000000;
IOLock gNvdecPower;

constexpr unsigned kJpgClock = 195;
constexpr uint32_t kSyncpt = 163;
}  // namespace

extern "C" {
uintptr_t ml_io_map(uintptr_t pa, uintptr_t)
{
    if (pa == 0x70000000) {
        static std::vector<uint8_t> apb(0x1000);
        apb[0x805] = 0x21;  // HIDREV: chip 0x21, major 2: T210B01
        apb[0x804] = 0x20;
        return reinterpret_cast<uintptr_t>(apb.data());
    }
    return reinterpret_cast<uintptr_t>(pa == 0x70019000 ? gMcRegisters.data() : gNvjpgRegisters.data());
}

int tegra_nvdec_await(uint32_t)
{
    return 0;
}

IOLock *tegra_nvdec_power_lock(void)
{
    return &gNvdecPower;
}

int tegra_nvjpg_publish(void)
{
    return 0;
}

int tegra_host1x_start(void)
{
    return 0;
}

int tegra_host1x_syncpt_alloc(uint32_t *id)
{
    *id = kSyncpt;
    return 0;
}

uint32_t tegra_host1x_syncpt_read(uint32_t)
{
    return gEngine.syncpt;
}

int tegra_host1x_syncpt_wait(uint32_t, uint32_t threshold, uint32_t)
{
    return static_cast<int32_t>(gEngine.syncpt - threshold) >= 0 ? 0 : TEGRA_HOST1X_ETIMEDOUT;
}

int tegra_host1x_syncpt_release_failed(uint32_t, uint32_t, uint32_t)
{
    return 0;
}

void tegra_host1x_set_strict(void)
{}

void tegra_host1x_set_sleep_client(void (*)(), void (*)())
{}

int tegra_host1x_submit_on(uint32_t channel, const uint32_t *words, uint32_t count)
{
    EXPECT_EQ(channel, uint32_t(TEGRA_HOST1X_CHANNEL_NVJPG));
    gEngine.streams.emplace_back(words, words + count);
    const bool decode = count > 3;
    for (uint32_t i = 0; i < count; i++) {
        if (words[i] == tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, kSyncpt) && gEngine.reachable())
            gEngine.syncpt++;
        if (words[i] == tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, kSyncpt) && gEngine.reachable() &&
            gEngine.falconRunning && !(decode && gEngine.stallDecodes))
            gEngine.syncpt++;
    }
    return 0;
}

int switch_host1x_job_begin(uint32_t channel, uint32_t capacity, uint32_t **words)
{
    static std::vector<uint32_t> arena;
    EXPECT_EQ(channel, uint32_t(TEGRA_HOST1X_CHANNEL_NVJPG));
    arena.assign(capacity, 0);
    *words = arena.data();
    return 0;
}

int switch_host1x_job_submit(uint32_t channel, const uint32_t *first, uint32_t count, uint32_t, uint32_t)
{
    return tegra_host1x_submit_on(channel, first, count);
}

void switch_host1x_job_cancel(uint32_t)
{}

int switch_host1x_job_copy(uint32_t channel, const uint32_t *words, uint32_t count, uint32_t, uint32_t)
{
    return tegra_host1x_submit_on(channel, words, count);
}

void tegra_host1x_describe_channel(uint32_t channel, char *buffer, size_t size)
{
    snprintf(buffer, size, "ch%u model", channel);
}

void tegra_host1x_channel_teardown(uint32_t)
{
    gEngine.teardowns++;
}

void tegra_host1x_channel_restart(uint32_t)
{
    gEngine.restarts++;
}

void tegra_mc_hot_reset(uint32_t ctrl, uint32_t bit, bool on)
{
    EXPECT_EQ(ctrl, 0x970u);
    EXPECT_EQ(bit, 1u << 8);
    gEngine.flushed = on;
    gEngine.events.push_back(on ? "flush" : "release");
}

uint32_t tegra_smmu_read(uint32_t offset)
{
    return offset == 0x974 && gEngine.flushed ? 1u << 8 : 0;
}

int tegra_smmu_attach(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, char *, size_t)
{
    return 0;
}

void tegra_smmu_detach(uint32_t, uint32_t)
{}

void tegra_smmu_flush(uint32_t)
{}

void switch_car_clock_enable(unsigned id)
{
    gEngine.clocks[id] = true;
    if (id == kJpgClock) {
        gEngine.clockOn = true;
        gEngine.events.push_back("clock on");
    }
}

void switch_car_clock_disable(unsigned id)
{
    gEngine.clocks[id] = false;
    if (id == kJpgClock) {
        gEngine.clockOn = false;
        gEngine.events.push_back("clock off");
    }
}

bool switch_car_clock_enabled(unsigned id)
{
    return gEngine.clocks[id];
}

void switch_car_reset_assert(unsigned id)
{
    gEngine.resets[id] = true;
    if (id == kJpgClock) {
        gEngine.inReset = true;
        gEngine.falconRunning = false;
        gEngine.events.push_back("reset");
    }
}

void switch_car_reset_deassert(unsigned id)
{
    gEngine.resets[id] = false;
    if (id == kJpgClock) {
        gEngine.inReset = false;
        gEngine.events.push_back("unreset");
    }
}

bool switch_car_reset_asserted(unsigned id)
{
    return gEngine.resets[id];
}

uint32_t switch_car_read(uint32_t)
{
    return 0;
}

void switch_car_write(uint32_t, uint32_t)
{}

void switch_car_rmw(uint32_t offset, uint32_t, uint32_t)
{
    if (offset == 0x554)
        gEngine.events.push_back("lvl2");
}

uint32_t switch_engine_clock_source(unsigned, uint32_t *khz)
{
    *khz = 408000;
    return 4u << 29;
}

bool switch_pmc_partition_is_on(unsigned partition)
{
    return partition == 25 ? gEngine.nvdecPartitionOn : gEngine.partitionOn;
}

bool switch_pmc_partition_on(unsigned partition)
{
    EXPECT_EQ(partition, 26u);
    gEngine.partitionOn = true;
    gEngine.clamped = true;
    gEngine.partitionPowerOns++;
    gEngine.events.push_back("partition on");
    return true;
}

bool switch_pmc_partition_off(unsigned partition)
{
    EXPECT_EQ(partition, 26u);
    EXPECT_FALSE(gEngine.nvdecPartitionOn) << "NVDEC's partition needs NVJPG's on";
    gEngine.partitionOn = false;
    gEngine.events.push_back("partition off");
    return true;
}

void switch_pmc_remove_clamping(unsigned)
{
    gEngine.clamped = false;
    gEngine.events.push_back("unclamp");
}

uint32_t switch_pmc_read(uint32_t offset)
{
    return offset == 0x2c && gEngine.clamped ? 1u << 26 : 0;
}

switch_falcon_result switch_falcon_load(volatile uint8_t *, uint64_t, const switch_falcon_image *)
{
    return gEngine.reachable() ? SWITCH_FALCON_OK : SWITCH_FALCON_SCRUBBING;
}

void switch_falcon_start(volatile uint8_t *)
{
    gEngine.boots++;
    gEngine.falconRunning = gEngine.reachable() && !gEngine.deadAfterReset;
}

const char *switch_falcon_state(const volatile uint8_t *, char *text, size_t size)
{
    snprintf(text, size, "falcon model");
    return text;
}

const char *switch_falcon_stall(volatile uint8_t *, char *text, size_t size)
{
    snprintf(text, size, "falcon model stalled");
    return text;
}
}

bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t, IOOptionBits)
{
    const size_t rounded = (size + 4095) & ~size_t(4095);
    out->va = static_cast<uint8_t *>(aligned_alloc(4096, rounded));
    memset(out->va, 0, rounded);
    out->pa = gNextPa;
    out->size = rounded;
    out->memory = nullptr;
    gNextPa += rounded;
    return true;
}

void switch_dma_free(switch_dma_buffer *buffer)
{
    free(buffer->va);
    buffer->va = nullptr;
}

namespace {
TegraNVJPGJob decodeJob()
{
    TegraNVJPGJob job = {};
    job.setup.frame_width = 64;
    job.setup.frame_height = 64;
    job.setup.bitstream_size = 4096;
    job.bitstream = 0x1000000;
    job.plane[0] = 0x2000000;
    job.plane[1] = 0x2100000;
    job.planes = 2;
    job.pixels = 64 * 64;
    job.selector = 7;
    return job;
}

void idle()
{
    thread_call_t timer = switch_test_calls().back();
    timer->fire(timer->param, nullptr);
}

bool logged(const char *text)
{
    const auto &lines = switch_test_log();
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(text) != std::string::npos;
    });
}

class NVJPGGate : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        ASSERT_EQ(tegra_nvjpg_probe(), 0);
        ASSERT_TRUE(tegra_nvjpg_ready());
    }

    void SetUp() override
    {
        gEngine.stallDecodes = false;
        gEngine.deadAfterReset = false;
        switch_test_log().clear();
    }
};
}  // namespace

TEST_F(NVJPGGate, AJobAfterTheIdlePowerOffPowersThePartitionOnBootsTheFalconAndRuns)
{
    EXPECT_EQ(gEngine.syncpt, 369427731u);  // J2's IMMEDIATE and OP_DONE
    size_t from = gEngine.events.size();
    idle();
    EXPECT_FALSE(gEngine.partitionOn);
    EXPECT_TRUE(gEngine.flushed) << "L4T keeps the client in hot reset while the partition is off";
    using Steps = std::vector<std::string>;
    EXPECT_EQ(gEngine.steps(from, {"clock on", "flush", "reset", "clock off", "partition off"}),
              (Steps{"clock on", "flush", "reset", "clock off", "partition off"}));
    from = gEngine.events.size();
    const int powerOns = gEngine.partitionPowerOns, boots = gEngine.boots;
    const size_t streams = gEngine.streams.size();
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    // powergate-t21x.c's order, then nvhost's module reset; no MBIST pulse on T210B01.
    EXPECT_EQ(gEngine.steps(from, {"partition on", "clock on", "reset", "unclamp", "unreset", "release", "lvl2"}),
              (Steps{"partition on", "clock on", "reset", "unclamp", "unreset", "release", "reset", "unreset"}));
    EXPECT_FALSE(gEngine.flushed);
    EXPECT_EQ(switch_mmio_read32(gNvjpgRegisters.data(), 0x117c), 0x30008u);  // FALCON_CG2's prod value
    EXPECT_EQ(gEngine.partitionPowerOns, powerOns + 1);
    EXPECT_FALSE(gEngine.clamped);
    EXPECT_EQ(gEngine.boots, boots + 1);
    ASSERT_EQ(gEngine.streams.size(), streams + 2);  // the power-on's proof of life, then the job
    EXPECT_EQ(gEngine.streams[streams].size(), 3u);
    const std::vector<uint32_t> &decode = gEngine.streams.back();
    EXPECT_EQ(decode.front(), tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVJPG, 0, 0));
    EXPECT_EQ(decode.back(), tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, kSyncpt));
    EXPECT_EQ(gEngine.syncpt, 369427734u);  // the proof's two increments, then the job's fence
}

TEST_F(NVJPGGate, AJobPastItsBoundIsCancelledAndTheNextJobRuns)
{
    idle();
    gEngine.stallDecodes = true;
    const int boots = gEngine.boots, teardowns = gEngine.teardowns, restarts = gEngine.restarts;
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -5);
    EXPECT_TRUE(logged("not reached"));
    EXPECT_TRUE(logged("job selector 7, 64x64"));
    EXPECT_TRUE(logged("engine reset: running again"));
    EXPECT_EQ(gEngine.teardowns, teardowns + 1);
    EXPECT_EQ(gEngine.restarts, restarts + 1);
    EXPECT_EQ(gEngine.boots, boots + 2);  // the gate's power-on, then the cancellation's reboot
    EXPECT_TRUE(gEngine.partitionOn);
    EXPECT_FALSE(gEngine.flushed);
    EXPECT_TRUE(logged("memory client drained"));
    EXPECT_TRUE(tegra_nvjpg_ready());

    gEngine.stallDecodes = false;
    const uint32_t before = gEngine.syncpt;
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_EQ(gEngine.syncpt, before + 1);
}

// powergate-t21x.c powers NVJPG's partition before NVDEC's and keeps it on while NVDEC's is: an idle NVJPG is then
// only flushed, reset and unclocked.
TEST_F(NVJPGGate, AnIdlePowerOffKeepsThePartitionOnWhileNVDECsIsOn)
{
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    gEngine.nvdecPartitionOn = true;
    idle();
    EXPECT_TRUE(gEngine.partitionOn);
    EXPECT_TRUE(gEngine.inReset);
    EXPECT_FALSE(gEngine.clockOn);
    EXPECT_TRUE(gEngine.flushed);
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    gEngine.nvdecPartitionOn = false;
}

// The stream is the job's: the selector the request came through only names it in a stall line. Two decodes of one
// picture differ in their PICTURE_INDEX value alone.
TEST_F(NVJPGGate, TheStreamIsTheSameWhicheverSelectorTheJobCameThrough)
{
    TegraNVJPGJob first = decodeJob(), seventh = decodeJob();
    first.selector = 1;
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&first, &status, nullptr), 0);
    const std::vector<uint32_t> one = gEngine.streams.back();
    ASSERT_EQ(tegra_nvjpg_run(&seventh, &status, nullptr), 0);
    const std::vector<uint32_t> seven = gEngine.streams.back();
    ASSERT_EQ(one.size(), seven.size());
    ASSERT_EQ(one.size(), 29u);  // SETCLASS, 9 methods of 3 words for two planes, OP_DONE: T-v3a's 0x74 bytes
    for (size_t i = 0; i < one.size(); i++) {
        if (i > 0 && one[i - 1] == NVE7D0_SET_PICTURE_INDEX / 4) {
            EXPECT_EQ(seven[i], one[i] + 1);
            continue;
        }
        EXPECT_EQ(one[i], seven[i]) << "word " << i;
    }
}

TEST_F(NVJPGGate, AnEngineTheResetDoesNotBringBackRefusesTheNextJob)
{
    gEngine.stallDecodes = true;
    gEngine.deadAfterReset = true;
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -5);
    EXPECT_TRUE(logged("it did not come back, NVJPG refuses work"));
    EXPECT_FALSE(tegra_nvjpg_ready());
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -19);
}
