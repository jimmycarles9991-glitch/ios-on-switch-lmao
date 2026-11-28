// SPDX-License-Identifier: MIT
// A packed state whose programs read a buffer's Metal address (pack v36 guestBaseWord, guestBaseSlot): its pipeline holds
// the push words and slots each draw writes that address to (RenderEncoder::guestBaseWords), as each of its program
// variants does and its stride twin holds its own record's, and the scan refuses words a draw could not write.
#include "facade_harness.h"
#include "pack_fixture_image.h"
#include "../pack_scan.h"

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char *kGuestBaseKey = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v guest_v  | f guest_f";
constexpr const char *kPlainKey = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v v  | f f";

TEST(PackGuestBases, APackedStatesMetalAddressWordsReachItsPipeline)
{
    const std::vector<uint8_t> file = pack_fixture::readFileBytes(pack_fixture::kPath);
    ASSERT_FALSE(file.empty());
    pack_fixture::configurePackImageFromBytes(file, true);
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(pack_fixture::kPath, 0, err, gm20b::Device::Role::Client)) << err;
    const auto made = [&dev](const char *key) -> gm20b::Pipeline * {
        const uint64_t h = gm20b::Pack::fnv(key, strlen(key));
        const gm20b::PackRun run = dev.pack().fastRun(h);
        return run.records ? dev.pipeline(run, h, gm20b::PackForm::Default, false) : nullptr;
    };
    const gm20b::Pipeline *guest = made(kGuestBaseKey);
    ASSERT_NE(guest, nullptr);
    EXPECT_EQ(guest->guestBases[0].stage, 0);
    EXPECT_EQ(guest->guestBases[0].slot, 5);
    EXPECT_EQ(guest->guestBases[0].word, 7);
    EXPECT_EQ(guest->guestBases[1].stage, 1);
    EXPECT_EQ(guest->guestBases[1].slot, 2);
    EXPECT_EQ(guest->guestBases[1].word, 3);
    EXPECT_EQ(guest->guestBases[2].word, 0);
    EXPECT_EQ(guest->pushMask & 0xccu, 0u);
    const gm20b::Pipeline *plain = made(kPlainKey);
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(plain->guestBases[0].word, 0);

    size_t variants = 0;
    ASSERT_NE(guest->variantTable, nullptr);
    for (const gm20b::Pipeline *variant : guest->variantTable->chosen) {
        if (variant == guest)
            continue;
        ++variants;
        for (uint32_t k = 0; k < gm20b::kGuestBases; k++) {
            EXPECT_EQ(variant->guestBases[k].stage, guest->guestBases[k].stage) << k;
            EXPECT_EQ(variant->guestBases[k].slot, guest->guestBases[k].slot) << k;
            EXPECT_EQ(variant->guestBases[k].word, guest->guestBases[k].word) << k;
        }
    }
    EXPECT_GT(variants, 0u) << "the state's program variants are placed";
    ASSERT_NE(guest->strideTwin, nullptr);
    EXPECT_EQ(guest->strideTwin->guestBases[0].stage, 0);
    EXPECT_EQ(guest->strideTwin->guestBases[0].slot, 1);
    EXPECT_EQ(guest->strideTwin->guestBases[0].word, 9);
    EXPECT_EQ(guest->strideTwin->guestBases[1].word, 0);
}

TEST(PackGuestBases, TheScanRefusesWordsADrawCannotWrite)
{
    gm20b::PackPipeline pp = {};
    EXPECT_TRUE(gm20b::usableGuestBases(pp));
    pp.guestBaseWord[0] = 7;
    pp.guestBaseSlot[0] = 5;
    pp.guestBaseWord[1] = 3;
    pp.guestBaseSlot[1] = gm20b::kGuestBaseFragment | 2;
    EXPECT_TRUE(gm20b::usableGuestBases(pp));
    const auto refused = [&pp](void (*change)(gm20b::PackPipeline &)) {
        gm20b::PackPipeline changed = pp;
        change(changed);
        return !gm20b::usableGuestBases(changed);
    };
    EXPECT_TRUE(refused([](gm20b::PackPipeline &p) {
        p.guestBaseWord[1] = 32;
    })) << "past the root push words";
    EXPECT_TRUE(refused([](gm20b::PackPipeline &p) {
        p.guestBaseSlot[0] = 31;
    })) << "past the buffer slots";
    EXPECT_TRUE(refused([](gm20b::PackPipeline &p) {
        p.guestBaseWord[1] = 8;
    })) << "a word another address writes";
    EXPECT_TRUE(refused([](gm20b::PackPipeline &p) {
        p.pushMask = 1u << 6;
    })) << "a word the record pushes";
    EXPECT_TRUE(refused([](gm20b::PackPipeline &p) {
        p.guestBaseWord[0] = 0;
    })) << "listed past the list's end";
}

}  // namespace
