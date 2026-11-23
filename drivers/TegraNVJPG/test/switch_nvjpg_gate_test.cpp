// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVJPG.cpp's idle gate, stall cancellation and job windows against the engine model
// (switch_nvjpg_engine_model.h).
#include "switch_nvjpg_engine_model.h"
#include "../TegraNVJPGRequest.h"

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

// The falcon OS raises an application error to the host and parks until it is cleared (TRM 15.4.1.7). NVJPG's own
// line (global 8, ch.3 p.33) wakes the decode at once; it fails with the firmware's reason, the error is cleared as
// nvhost clears it, the engine reset and rebooted with its line off meanwhile, and the next decode runs.
TEST_F(NVJPGGate, AFalconErrorFailsTheDecodeAtOnceWithTheFirmwaresReason)
{
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    gEngine.falconErrorInDecode = true;
    gEngine.stallDecodes = true;
    gEngine.decodeWaitEnded.clear();
    const size_t from = gEngine.events.size();
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -5);
    EXPECT_EQ(gEngine.decodeWaitEnded, "falcon") << "the decode ends on the error, not at its bound";
    gEngine.falconErrorInDecode = false;
    gEngine.stallDecodes = false;
    EXPECT_TRUE(logged("falcon error interrupt: MAILBOX0 0x8 (MSDECOS_ERROR_APPLICATION) MAILBOX1 0x77"));
    EXPECT_FALSE(logged("not reached in"));
    EXPECT_TRUE(logged("engine reset: running again"));
    using Steps = std::vector<std::string>;
    EXPECT_EQ(gEngine.steps(from, {"falcon error", "kick", "flush", "reset", "falcon start"}),
              (Steps{"falcon error", "kick", "flush", "reset", "reset", "reset", "falcon start"}));
    EXPECT_EQ(switch_mmio_read32(gNvjpgRegisters.data(), SWITCH_FALCON_THI_INT_STATUS), 0u);
    EXPECT_TRUE(IOWorkLoop::last()->enabled) << "the line is on again once the falcon is proven";
    const uint32_t before = gEngine.syncpt;
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_EQ(gEngine.syncpt, before + 1);
}

// "Whenever a module is reset, the corresponding teardown bit in [HOST1X_SYNC_MOD_TEARDOWN] should be written" (TRM
// 14.9.14): every power-on, the idle gate's and a cancellation's, tears down NVJPG's module (bit 14) after its last
// reset and before its falcon starts.
TEST_F(NVJPGGate, EveryResetIsFollowedByHost1xsModuleTeardownBeforeTheFalconStarts)
{
    idle();
    size_t from = gEngine.events.size();
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    using Steps = std::vector<std::string>;
    EXPECT_EQ(gEngine.steps(from, {"reset", "unreset", "module teardown 14", "falcon start"}),
              (Steps{"reset", "unreset", "reset", "unreset", "module teardown 14", "falcon start"}));

    gEngine.stallDecodes = true;
    from = gEngine.events.size();
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -5);
    EXPECT_EQ(gEngine.steps(from, {"reset", "unreset", "module teardown 14", "falcon start"}),
              (Steps{"reset", "reset", "unreset", "reset", "unreset", "module teardown 14", "falcon start"}));
    gEngine.stallDecodes = false;
}

// The TRM's NVJPG ungating (12.2.7.10 steps 3-4) starts NVJPG's and NVDEC's clocks and stops NVDEC's before the
// clamps go, on T210B01 as on T210: NVDEC part A sits in NVJPG's partition. A partition already on (NVDEC's keeps it
// on) is not ungated, and NVDEC's clock is left as found.
TEST_F(NVJPGGate, UngatingThePartitionPulsesNVDECsClockBeforeTheClampsGo)
{
    idle();
    size_t from = gEngine.events.size();
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    using Steps = std::vector<std::string>;
    EXPECT_EQ(gEngine.steps(from, {"partition on", "clock on", "nvdec clock on", "nvdec clock off", "unclamp", "lvl2"}),
              (Steps{"partition on", "clock on", "nvdec clock on", "nvdec clock off", "unclamp"}));
    EXPECT_FALSE(gEngine.clocks[kNvdecClock]);

    gEngine.nvdecPartitionOn = true;
    gEngine.clocks[kNvdecClock] = true;
    idle();
    from = gEngine.events.size();
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_TRUE(gEngine.steps(from, {"nvdec clock on", "nvdec clock off"}).empty());
    EXPECT_TRUE(gEngine.clocks[kNvdecClock]) << "NVDEC's running clock is not touched";
    gEngine.nvdecPartitionOn = false;
    gEngine.clocks[kNvdecClock] = false;
}

// Every partition procedure of the TRM (12.2.7) waits for the partition's REMOVE_CLAMPING_CMD bit to clear; CLAMP_STATUS
// is kept only for backward compatibility (12.6.12). A power-on whose CLAMP_STATUS reads set once the command has
// cleared comes up, and one whose command never clears fails with both registers in its line.
TEST_F(NVJPGGate, APowerOnWaitsForTheRemoveClampingCommandNotTheLegacyClampStatus)
{
    idle();
    gEngine.clampStatusStale = true;
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_FALSE(logged("power-on FAIL"));
    gEngine.clampStatusStale = false;

    idle();
    gEngine.clampStuck = true;
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -19);
    EXPECT_TRUE(logged("did not come up (PWRGATE_STATUS 0 REMOVE_CLAMPING_CMD 0x4000000 CLAMP_STATUS 0x4000000"));
    EXPECT_TRUE(
        logged("TegraNVJPG: partition 26 kept its clamps: REMOVE_CLAMPING_CMD 0x4000000 CLAMP_STATUS 0x4000000"));
    EXPECT_FALSE(gEngine.partitionOn) << "powered back off, so the next power-up ungates it again";
    gEngine.clampStuck = false;
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0) << "the next decode powers it on again";
}

// A partition already on (NVDEC's keeps NVJPG's on, or the boot chain left it on) has no clamp to remove: no command is
// sent to it, so a REMOVE_CLAMPING_CMD bit that would stay set for such a partition cannot fail the power-on.
TEST_F(NVJPGGate, APartitionAlreadyOnIsNotSentTheRemoveClampingCommand)
{
    gEngine.nvdecPartitionOn = true;
    gEngine.removeSticksWhenUnclamped = true;
    idle();
    const size_t from = gEngine.events.size();
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_TRUE(gEngine.steps(from, {"unclamp"}).empty());
    EXPECT_FALSE(logged("power-on FAIL"));
    gEngine.nvdecPartitionOn = false;
    gEngine.removeSticksWhenUnclamped = false;
    gEngine.removePending = false;
}

// STATUS reads done for a client with no flush requested, so a request another writer dropped looks drained unless
// the request bit is read with it (TRM 18.11.1.114): the cancellation says the client was not flushed.
TEST_F(NVJPGGate, ACancellationWhoseFlushRequestWasDroppedSaysTheClientWasNotFlushed)
{
    gEngine.stallDecodes = true;
    gEngine.requestDropped = true;
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -5);
    EXPECT_TRUE(
        logged("TegraNVJPG: MC hot reset request bit 0x100 of MC 0x970 reads clear while its flush was awaited"));
    EXPECT_TRUE(logged("memory client did not drain")) << "the line must not say drained";
    EXPECT_FALSE(logged("memory client drained"));
    gEngine.stallDecodes = false;
    gEngine.requestDropped = false;
}

// Channel 3 is NVJPG's alone and one job runs at a time, so a job waits only on itself: 50 ms plus a microsecond per 16
// pixels of its MCU grid. SpringBoard's 205x83 file that hung the engine is cancelled in 51 ms (it took 2 s).
TEST_F(NVJPGGate, AJobIsBoundedByItsOwnPixelsAndASmallMargin)
{
    TegraNVJPGJob job = decodeJob();
    job.setup.frame_width = 205;
    job.setup.frame_height = 83;
    job.pixels = 208 * 88;
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_EQ(gEngine.lastBoundUs, 50000u + 208 * 88 / 16);
    job.pixels = 4032ull * 3024;
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_EQ(gEngine.lastBoundUs, 50000u + 4032 * 3024 / 16);
    job.pixels = 16384ull * 16384 * 2;
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_EQ(gEngine.lastBoundUs, 30000000u) << "at most 30 s";
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

// An engine the cancellation's reset does not bring back is off to the idle gate: each later decode runs the normal
// power-on (a failed one fails that decode, with its line), and the first whose power-on boots the engine runs.
TEST_F(NVJPGGate, AnEngineTheResetDoesNotBringBackIsPoweredOnByALaterDecode)
{
    gEngine.stallDecodes = true;
    gEngine.deadAfterReset = true;
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -5);
    EXPECT_TRUE(logged("it did not come back, the next decode powers it on again"));
    EXPECT_TRUE(tegra_nvjpg_ready()) << "the service keeps taking decodes";
    EXPECT_FALSE(gEngine.clockOn);
    const int boots = gEngine.boots;
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -19);
    EXPECT_EQ(gEngine.boots, boots + 1) << "the next decode tried a power-on";
    EXPECT_TRUE(logged("power-on FAIL"));
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -19);
    EXPECT_EQ(gEngine.boots, boots + 2) << "the decode after it runs the power-on again, with no wait";
    gEngine.stallDecodes = false;
    gEngine.deadAfterReset = false;
    const uint32_t before = gEngine.syncpt;
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_EQ(gEngine.boots, boots + 3);
    EXPECT_EQ(gEngine.syncpt, before + 3) << "the power-on's proof, then the decode";
}

// Every DCT scale ships (AppleJPEGSupportsDCTScaling answers 1): CMPhoto's quarter and eighth thumbnails of a photo plan
// on NVJPG with the published traits.
TEST_F(NVJPGGate, ThePublishedTraitsTakeEveryDCTScale)
{
    const TegraNVJPGTraits *t = tegra_nvjpg_traits();
    ASSERT_EQ(t->scales, 0xfu);
    const TegraNVJPGAbility shipped = {t->formats, t->chroma, t->scales, t->clipped ? 1u : 0u, t->greyRgb ? 1u : 0u};
    TegraNVJPGImage photo = {};
    photo.width = 4032, photo.height = 3024, photo.components = 3, photo.chroma = TEGRA_NVJPG_CHROMA_420;
    photo.mcuW = photo.mcuH = 16, photo.mcusX = 252, photo.mcusY = 189;
    for (uint32_t scale = 0; scale < 4; scale++) {
        TegraNVJPGDecodeRequest r = {};
        r.dst_size = 1u << 24, r.width = 4032, r.height = 3024, r.subsampling = TEGRA_NVJPG_SUB_420;
        r.out_width = 4032 >> scale, r.out_height = 3024 >> scale;
        TegraNVJPGSurface d = {};
        d.fourcc = TEGRA_NVJPG_FOURCC('B', 'G', 'R', 'A'), d.width = r.out_width, d.height = r.out_height;
        d.planeBytesPerRow[0] = (4ull * r.out_width + 63) & ~63ull, d.planeHeight[0] = r.out_height;
        d.length = d.planeBytesPerRow[0] * r.out_height;
        TegraNVJPGPlan plan;
        uint32_t kind = 99;
        EXPECT_EQ(tegra_nvjpg_plan(&r, &photo, &d, &shipped, 1u << 20, &plan, &kind), nullptr) << "1/" << (1u << scale);
        EXPECT_EQ(plan.target.scale, scale);
    }
}

// A decode's host1x submit and syncpoint wait take no lock: the job's use of the gate keeps the engine on.
TEST_F(NVJPGGate, AJobWaitsForTheEngineWithNoStateLockHeld)
{
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    gEngine.waitsUnderStateLock = 0;
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    // base 2: the submit's wait for room and the job's syncpoint wait, both under gStateLock
    EXPECT_EQ(gEngine.waitsUnderStateLock, 0);
}

// The cancellation takes the power lock with gStateLock not held, so the only order between the two is the gate's
// (power lock, then gateOn/gateOff's gStateLock).
TEST_F(NVJPGGate, ACancellationTakesThePowerLockOutsideTheStateLock)
{
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    gEngine.waitsUnderStateLock = gEngine.powerUnderStateLock = 0;
    switch_test_lock_taken = [](IOLock *lock) {
        gEngine.powerUnderStateLock += lock == &gNvdecPower && gStateLock->locked;
    };
    gEngine.stallDecodes = true;
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -5);
    switch_test_lock_taken = nullptr;
    EXPECT_TRUE(logged("engine reset: running again"));
    // base 1: the power lock inside gStateLock; base 4: the job's submit and wait and the reboot's proof under gStateLock
    EXPECT_EQ(gEngine.powerUnderStateLock, 0);
    EXPECT_EQ(gEngine.waitsUnderStateLock, 0);
}

TEST_F(NVJPGGate, AllocationsKeepDisjointMappingsUntilTheirOwnRelease)
{
    constexpr uint32_t kWindow = TEGRA_NVJPG_WINDOW_OUT;
    const uint64_t base = kTegraNVJPGWindowBase[kWindow];
    const uint64_t tablesPa = gTables.pa + (1 + kWindowTables[0]) * 4096ULL;
    std::vector<uint64_t> pages(300);
    for (uint32_t i = 0; i < pages.size(); ++i)
        pages[i] = 0x90000000ULL + i * 4096ULL;
    gSmmuRangeFlushes.clear();
    gSmmuFlushAlls = 0;

    const uint64_t first = tegra_nvjpg_pages_map(kWindow, pages.data(), 300, 0, true);
    ASSERT_EQ(first, base);
    for (uint32_t i = 0; i < 300; ++i)
        ASSERT_EQ(gDomain.translate(first + i * 4096ULL), pages[i]) << "page " << i;
    EXPECT_EQ(gDomain.translate(first + 300 * 4096ULL), UINT64_MAX);
    ASSERT_EQ(gSmmuRangeFlushes.size(), 1u);
    SmmuRangeFlush f = gSmmuRangeFlushes.back();
    EXPECT_EQ(f.asid, uint32_t(TEGRA_SMMU_ASID_NVJPG));
    EXPECT_EQ(f.tablesPa, tablesPa);
    EXPECT_EQ(f.tableBytes, 300u * 4);
    EXPECT_EQ(f.iova, first);
    EXPECT_EQ(f.iovaBytes, 300u * 4096);

    const uint64_t second = tegra_nvjpg_pages_map(kWindow, pages.data(), 100, 0, false);
    ASSERT_EQ(second, first + TegraSMMUSpace::round(300u * 4096) + TegraSMMUSpace::kGranule);
    EXPECT_EQ(gDomain.translate(first + 299 * 4096ULL), pages[299]);
    EXPECT_EQ(gDomain.translate(second + 99 * 4096ULL), pages[99]);
    ASSERT_EQ(gSmmuRangeFlushes.size(), 2u);
    EXPECT_EQ(gSmmuRangeFlushes.back().iova, second);
    EXPECT_EQ(gSmmuRangeFlushes.back().iovaBytes, 100u * 4096);

    tegra_nvjpg_pages_unmap(kWindow, first, 300);
    EXPECT_EQ(gDomain.translate(first), UINT64_MAX);
    EXPECT_EQ(gDomain.translate(second), pages[0]);
    ASSERT_EQ(gSmmuRangeFlushes.size(), 3u);
    EXPECT_EQ(gSmmuRangeFlushes.back().iovaBytes, 300u * 4096);
    EXPECT_EQ(tegra_nvjpg_pages_map(kWindow, pages.data(), 300, 0, true), first);
    tegra_nvjpg_pages_unmap(kWindow, first, 300);
    tegra_nvjpg_pages_unmap(kWindow, second, 100);
    EXPECT_EQ(gSmmuFlushAlls, 0u);
}

// Every process's destinations share the output window, packed back to back: a write past one mapping must fault in
// the SMMU, never land in the next one.
TEST_F(NVJPGGate, AnUnmappedGranuleFollowsEveryMapping)
{
    constexpr uint32_t kWindow = TEGRA_NVJPG_WINDOW_OUT;
    constexpr uint32_t kGranulePages = TegraSMMUSpace::kGranule / 4096;
    std::vector<uint64_t> pages(kGranulePages);
    for (uint32_t i = 0; i < kGranulePages; ++i)
        pages[i] = 0x90000000ULL + i * 4096ULL;
    const uint64_t first = tegra_nvjpg_pages_map(kWindow, pages.data(), kGranulePages, 0, true);
    const uint64_t second = tegra_nvjpg_pages_map(kWindow, pages.data(), kGranulePages, 0, true);
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);
    EXPECT_EQ(second - first, 2 * TegraSMMUSpace::kGranule);
    for (uint64_t at = first + TegraSMMUSpace::kGranule; at < second; at += 4096)
        EXPECT_EQ(gDomain.translate(at), UINT64_MAX) << "guard page " << (at - first) / 4096;
    tegra_nvjpg_pages_unmap(kWindow, first, kGranulePages);
    tegra_nvjpg_pages_unmap(kWindow, second, kGranulePages);
    EXPECT_EQ(gSpaces[kWindow].bytesFree(), gSpaces[kWindow].bytesTotal());
}

// The sink entries after a mapping's pages all point at the one arena page, writable only where the pages are.
TEST_F(NVJPGGate, SinkEntriesFollowThePagesAndThenTheGuard)
{
    const std::vector<uint64_t> pages{0x90000000, 0x90002000};
    const uint64_t out = tegra_nvjpg_pages_map(TEGRA_NVJPG_WINDOW_OUT, pages.data(), 2, 3, true);
    const uint64_t in = tegra_nvjpg_pages_map(TEGRA_NVJPG_WINDOW_IN, pages.data(), 2, 1, false);
    ASSERT_NE(out, 0u);
    ASSERT_NE(in, 0u);
    EXPECT_EQ(gDomain.translate(out + 4096), pages[1]);
    for (uint64_t page = 2; page < 5; ++page)
        EXPECT_EQ(gDomain.translate(out + page * 4096), gSink.pa) << "sink entry " << page;
    EXPECT_EQ(gDomain.translate(out + 5 * 4096), UINT64_MAX);
    EXPECT_NE(*gDomain.pte(out + 4 * 4096, false) & TegraSMMUDomain::kWrite, 0u);
    EXPECT_EQ(gDomain.translate(in + 2 * 4096), gSink.pa);
    EXPECT_EQ(*gDomain.pte(in + 2 * 4096, false) & TegraSMMUDomain::kWrite, 0u);
    EXPECT_EQ(gDomain.translate(in + 3 * 4096), UINT64_MAX);
    tegra_nvjpg_pages_unmap(TEGRA_NVJPG_WINDOW_OUT, out, 5);
    tegra_nvjpg_pages_unmap(TEGRA_NVJPG_WINDOW_IN, in, 3);
    EXPECT_EQ(gDomain.translate(out + 4 * 4096), UINT64_MAX);
    EXPECT_EQ(gDomain.translate(in + 2 * 4096), UINT64_MAX);
}

// An unmap the window's allocator does not recognise (a wrong window, or an address inside another mapping) clears
// no page entry: they may belong to a live surface.
TEST_F(NVJPGGate, AnUnmapTheWindowDoesNotHoldLeavesThePageEntries)
{
    const std::vector<uint64_t> pages{0x90000000, 0x90001000, 0x90002000};
    const uint64_t address = tegra_nvjpg_pages_map(TEGRA_NVJPG_WINDOW_OUT, pages.data(), 3, 0, true);
    ASSERT_NE(address, 0u);
    const size_t flushes = gSmmuRangeFlushes.size();
    tegra_nvjpg_pages_unmap(TEGRA_NVJPG_WINDOW_OUT, address + 4096, 1);
    tegra_nvjpg_pages_unmap(TEGRA_NVJPG_WINDOW_IN, address, 3);
    for (uint32_t i = 0; i < 3; ++i)
        EXPECT_EQ(gDomain.translate(address + i * 4096ULL), pages[i]) << "page " << i;
    EXPECT_EQ(gSmmuRangeFlushes.size(), flushes);
    EXPECT_TRUE(logged("which window 1 does not hold"));
    EXPECT_TRUE(logged("which window 0 does not hold"));
    tegra_nvjpg_pages_unmap(TEGRA_NVJPG_WINDOW_OUT, address, 3);
    EXPECT_EQ(gDomain.translate(address), UINT64_MAX);
}
