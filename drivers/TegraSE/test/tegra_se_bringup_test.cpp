// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraSE's bring-up against a register model of both engines: an operation the model never ends turns its engine off
// for the boot, and nothing after it waits for that engine; once bring-up is over, a caller's operation that never ends
// is a panic. Each scenario runs in a process of its own (the driver's state is boot-wide) and is given 5 s; one still
// waiting then is let go (callers refused as at system sleep) and fails.
#include "tegra_se_model.h"
#include "bazel/testing/fresh_process.h"

namespace se_model {
uint32_t initial[16] = {1, 2, 3, 4, 5, 6, 7, 8};

void compress(ccdigest_state_t state, size_t nblocks, const void *)
{
    reinterpret_cast<uint32_t *>(state)[0] += static_cast<uint32_t>(nblocks);
}

const ccdigest_info kSha256 = {32, 32, 64, 0, nullptr, initial, compress};
const ccdigest_info kSha1 = {20, 20, 64, 0, nullptr, initial, compress};
}  // namespace se_model

namespace {
void bringUpWithSe2LostOnItsSecondOperation()
{
    se_model::engines[1].hangAt = 2;
    const bool ended = se_model::finishes([] {
        seThread(nullptr, THREAD_AWAKENED);
    });
    EXPECT_TRUE(ended) << "bring-up kept waiting for SE2 after it went off:\n" << se_model::log();
    EXPECT_TRUE(gReady);
    EXPECT_TRUE(gEngines[0].up);
    EXPECT_FALSE(gEngines[1].up);
    EXPECT_EQ(se_model::engines[1].started, 2u) << "an operation reached SE2 after it was off";
    EXPECT_TRUE(gEngines[0].proven) << "callers reach SE1 from here";
    EXPECT_EQ(gSmmuHolds[0], 0) << "every operation SE1 ended gave its holds back";
    EXPECT_NE(se_model::log().find("SE2 off for this boot: operation never ended"), std::string::npos)
        << se_model::log();
    ASSERT_FALSE(se_model::engines[0].crypto.empty());
    ASSERT_FALSE(se_model::engines[1].crypto.empty());
    for (const uint32_t config : se_model::engines[0].crypto)
        EXPECT_NE(config & kCryptoMccif, 0u) << "SE1 moves its data through its memory clients";
    for (const uint32_t config : se_model::engines[1].crypto)
        EXPECT_EQ(config & kCryptoMccif, 0u) << "SE2 stays on the AHB";
    EXPECT_EQ(gSmmuAttached[0], 1u);
    EXPECT_EQ(gSmmuAttached[1], 0u) << "SE2's swgroups are not written: its AHB traffic is PPCS";
    EXPECT_NE(se_model::log().find("SE2 DMA over the AHB (PPCS)"), std::string::npos) << se_model::log();
}

void stateLoadAfterItsEngineWentOff()
{
    gAhb = switch_mmio_map(kAhbArbPA, 0x1000);
    gMc = switch_mmio_map(kMcPA, 0x1000);
    gQueue = IOLockAlloc();
    ASSERT_TRUE(engineUp(gEngines[0]));
    ASSERT_TRUE(engineUp(gEngines[1]));
    se_model::engines[0].hangAt = 1;
    bool sha256 = true;
    bool sha1 = true;
    const bool ended = se_model::finishes([&] {
        sha256 = stateLoadWorks(gEngines[0], SHA256, &se_model::kSha256);
        sha1 = stateLoadWorks(gEngines[0], SHA1, &se_model::kSha1);
    });
    EXPECT_TRUE(ended) << "the SHA-1 state load kept waiting for SE1 after it went off:\n" << se_model::log();
    EXPECT_FALSE(sha256);
    EXPECT_FALSE(sha1);
    EXPECT_FALSE(gEngines[0].up);
    EXPECT_TRUE(gEngines[1].up);
    EXPECT_EQ(se_model::engines[0].started, 1u);
    EXPECT_EQ(se_model::engines[1].started, 0u) << "SE2 was not the engine under test";
    EXPECT_GT(gSmmuHolds[0], 0) << "the operation SE1 never ended keeps its pages held: SE1 may still write them";
}

void engineUpOnly(unsigned index)
{
    gAhb = switch_mmio_map(kAhbArbPA, 0x1000);
    gMc = switch_mmio_map(kMcPA, 0x1000);
    gQueue = IOLockAlloc();
    ASSERT_TRUE(engineUp(gEngines[index]));
}

// An error interrupt can end an operation with its writes still queued: its pages are let go only once they drained.
void anErrorWithWritesStillQueued()
{
    engineUpOnly(0);
    se_model::engines[0].errorAt = 1;
    se_model::engines[0].busyAfterError = 3;
    const bool ended = se_model::finishes([] {
        (void)stateLoadWorks(gEngines[0], SHA256, &se_model::kSha256);
    });
    EXPECT_TRUE(ended) << se_model::log();
    EXPECT_EQ(se_model::engines[0].started, 1u);
    EXPECT_EQ(gReleasedBusy, 0u) << "a list released while SE1 still showed busy";
    EXPECT_EQ(gSmmuHolds[0], 0) << "the operation ended: its holds went back";
    EXPECT_TRUE(gEngines[0].up) << "an operation error is the operation's, not the engine's";
}

// An engine turned off before its start reached nothing: the operation's holds go back.
void listsThatDoNotTake()
{
    engineUpOnly(0);
    se_model::engines[0].listsDoNotTake = true;
    const bool ended = se_model::finishes([] {
        (void)stateLoadWorks(gEngines[0], SHA256, &se_model::kSha256);
    });
    EXPECT_TRUE(ended) << se_model::log();
    EXPECT_FALSE(gEngines[0].up);
    EXPECT_EQ(se_model::engines[0].started, 0u);
    EXPECT_EQ(gSmmuHolds[0], 0) << "nothing started, so nothing stays held";
}

// A caller's operation on a proven engine may have DMA in flight into the caller's pages: never an engine turned off.
void callerOperationThatNeverEnds()
{
    ASSERT_TRUE(se_model::finishes([] {
        seThread(nullptr, THREAD_AWAKENED);
    }));
    ASSERT_TRUE(gReady);
    ASSERT_TRUE(gEngines[0].proven);
    gAlg[SHA256].on = true;  // the model's engines give no real digests, so no known answer passed
    switch_dma_buffer caller;
    ASSERT_TRUE(switch_dma_alloc(&caller, kBulk, 0, 0));
    se_model::engines[0].hangFromBytes = 1;
    uint8_t digest[32];
    (void)tegra_se_digest(256, caller.va, kBulk, digest);
    ADD_FAILURE() << "a caller's operation that never ended returned:\n" << se_model::log();
}
}  // namespace

TEST(TegraSEBringUp, AnEngineThatNeverEndsAnOperationIsOffAndBringUpFinishesWithoutIt)
{
    switch_ios::test::ExpectPassesInFreshProcess(bringUpWithSe2LostOnItsSecondOperation);
}

TEST(TegraSEBringUp, AStateLoadCheckOfAnEngineThatWentOffEndsAtOnce)
{
    switch_ios::test::ExpectPassesInFreshProcess(stateLoadAfterItsEngineWentOff);
}

TEST(TegraSEBringUp, AnOperationErrorDrainsTheEngineBeforeItsPagesAreLetGo)
{
    switch_ios::test::ExpectPassesInFreshProcess(anErrorWithWritesStillQueued);
}

TEST(TegraSEBringUp, AnEngineOffBeforeItsStartKeepsNothingHeld)
{
    switch_ios::test::ExpectPassesInFreshProcess(listsThatDoNotTake);
}

TEST(TegraSEBringUp, ACallersOperationThatNeverEndsOnAProvenEngineIsAPanic)
{
    switch_ios::test::ExpectExitInFreshProcess(callerOperationThatNeverEnds, 3,
                                               "panic: TegraSE: SE1 operation never ended");
}

namespace {
void chunkedInPlaceAesAndConditionWakeup()
{
    ASSERT_TRUE(se_model::finishes([] {
        seThread(nullptr, THREAD_AWAKENED);
    }));
    ASSERT_TRUE(gReady);
    gAlg[CBC].on = true;
    std::vector<uint8_t> buf(2 * kBounceBytes, 0x5a);
    uint8_t iv[16] = {1, 2, 3, 4};
    const unsigned before = se_model::engines[0].started + se_model::engines[1].started;
    gForced[0] = current_thread();
    const int rc = tegra_se_aes(TEGRA_SE_CBC, true, kKey, 16, iv, buf.data(), buf.data(), buf.size());
    gForced[0] = nullptr;
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(se_model::engines[0].started + se_model::engines[1].started - before, 2u);
    EXPECT_EQ(gSmmuHolds[0], 0);
    ASSERT_TRUE(tegra_se_claim(1, 0x1234, 100));
    std::thread releaser([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        tegra_se_release(1, 0x1234);
    });
    EXPECT_TRUE(tegra_se_claim(1, 0x5678, 1000));
    tegra_se_release(1, 0x5678);
    releaser.join();
}
}  // namespace

TEST(TegraSEBringUp, ChunkedInPlaceAesAndConditionWakeup)
{
    switch_ios::test::ExpectPassesInFreshProcess(chunkedInPlaceAesAndConditionWakeup);
}

namespace {
void bootSkipsBounceBuffersAndDigestHook()
{
    ASSERT_TRUE(se_model::finishes([] {
        seThread(nullptr, THREAD_AWAKENED);
    }));
    ASSERT_TRUE(gReady);
    for (unsigned i = 0; i < kEngines; ++i) {
        EXPECT_EQ(gEngines[i].bounceIn.va, nullptr);
        EXPECT_EQ(gEngines[i].bounceOut.va, nullptr);
        EXPECT_EQ(gSmmuOwnedAdded[i], 0u);
    }
    EXPECT_EQ(switch_digest_compress_hook, nullptr);
}

void inPlaceAndAliasedCbcDecryptPreservesIv()
{
    ASSERT_TRUE(se_model::finishes([] {
        seThread(nullptr, THREAD_AWAKENED);
    }));
    ASSERT_TRUE(gReady);
    gAlg[CBC].on = true;
    se_model::engines[0].fillOut = 0xaa;
    se_model::engines[1].fillOut = 0xaa;

    switch_dma_buffer buf;
    ASSERT_TRUE(switch_dma_alloc(&buf, 4096, 0, 0));
    uint8_t expectedIv[16];
    for (unsigned i = 0; i < 16; ++i)
        expectedIv[i] = static_cast<uint8_t>(0x10 + i);
    memset(buf.va, 0x55, 4096);
    memcpy(buf.va + 4096 - 16, expectedIv, 16);

    uint8_t iv[16] = {};
    gForced[0] = current_thread();
    forceOnly(&gEngines[0]);
    EXPECT_EQ(tegra_se_aes(TEGRA_SE_CBC, false, kKey, 16, iv, buf.va, buf.va, 4096), 0);
    EXPECT_EQ(memcmp(iv, expectedIv, 16), 0);
    EXPECT_EQ(buf.va[4096 - 1], 0xaa);
    EXPECT_NE(gEngines[0].bounceIn.va, nullptr);
    EXPECT_NE(gEngines[0].bounceOut.va, nullptr);
    EXPECT_EQ(gSmmuOwnedAdded[0], 2u);

    std::vector<uint8_t> aliasMirror(4096, 0x77);
    memcpy(aliasMirror.data() + 4096 - 16, expectedIv, 16);
    se_model::aliases.push_back({reinterpret_cast<uintptr_t>(aliasMirror.data()), static_cast<uint32_t>(buf.pa), 4096});
    memset(buf.va, 0x77, 4096);
    memcpy(buf.va + 4096 - 16, expectedIv, 16);
    memset(iv, 0, sizeof iv);
    EXPECT_EQ(tegra_se_aes(TEGRA_SE_CBC, false, kKey, 16, iv, aliasMirror.data(), buf.va, 4096), 0);
    EXPECT_EQ(memcmp(iv, expectedIv, 16), 0);
    EXPECT_EQ(buf.va[4096 - 1], 0xaa);

    forceOnly(nullptr);
    gForced[0] = nullptr;
    se_model::aliases.clear();
    switch_dma_free(&buf);
}
}  // namespace

TEST(TegraSEBringUp, BootSkipsBounceBuffersAndDigestHook)
{
    switch_ios::test::ExpectPassesInFreshProcess(bootSkipsBounceBuffersAndDigestHook);
}

TEST(TegraSEBringUp, InPlaceAndAliasedCbcDecryptPreservesIv)
{
    switch_ios::test::ExpectPassesInFreshProcess(inPlaceAndAliasedCbcDecryptPreservesIv);
}
