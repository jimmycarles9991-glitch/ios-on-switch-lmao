// SPDX-License-Identifier: MIT
#include "../compute_lookup.h"
#include "../compute_record.h"
#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

namespace {

template <typename Make>
gm20b::ComputeKernelResult<int> immediate(gm20b::ComputeKernelCache<int> &cache,
                                          const gm20b::ComputeSpecialization &key, Make make)
{
    auto job = cache.request(key, [&](auto job) {
        job->publish(make());
    });
    return {job->value(), job->retryable()};
}

gm20b::BoundTexture texture(uint32_t slot = 0)
{
    return {slot, 80, 2, 1, 1, {0, 1, 2, 3}};
}

gm20b::BoundSampler sampler(uint32_t slot = 0)
{
    return {slot, 1, 1, 0, 0, 0, 0, 1, 7, 0, 0.0f, 16.0f, true, false};
}

TEST(GM20BComputeLookup, BindingOrderAndClearedSlotsHaveCanonicalKeys)
{
    gm20b::ComputeSpecialization a, b;
    auto low = texture(1), high = texture(7);
    a.bindTexture(7, &high);
    a.bindTexture(1, &low);
    b.bindTexture(1, &low);
    b.bindTexture(7, &high);
    a.bindMemory(4, 32);
    a.bindMemory(0, 16);
    b.bindMemory(0, 16);
    b.bindMemory(4, 32);
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a.bindTexture(1, &low));
    EXPECT_FALSE(a.bindMemory(4, 32));
    EXPECT_TRUE(a.bindTexture(7, nullptr));
    EXPECT_TRUE(b.bindTexture(7, nullptr));
    EXPECT_TRUE(a.bindMemory(4, 0));
    EXPECT_TRUE(b.bindMemory(4, 0));
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a.bindTexture(7, nullptr));
    EXPECT_FALSE(a.bindMemory(4, 0));
}

TEST(GM20BComputeLookup, TextPreservesPackAndJournalIdentity)
{
    gm20b::ComputeSpecialization key;
    auto t = texture(3);
    auto s = sampler(5);
    key.bindTexture(3, &t);
    key.bindSampler(5, &s);
    key.bindMemory(2, 48);
    const auto text = key.text();
    EXPECT_EQ(text.memory, "2:48");
    EXPECT_EQ(text.textures, "3:80:2:1:1:00010203");
    EXPECT_EQ(text.samplers, "5:1:1:0:0:0:0:0:41800000:1:7:0:1:0");
}

// An RGB10A8_2P_XR10 texture held in two planes (xr_pass.h) selects the kernel that reads and writes its planes
// apart, named by ":xr2p" after its slot's fields; one of one image keeps its key and text, byte for byte
TEST(GM20BComputeLookup, ATwoPlaneTextureSelectsItsOwnKernelAndKeyText)
{
    gm20b::BoundTexture one = {2, 550, 2, 1, 1, {2, 3, 4, 5}};
    gm20b::BoundTexture planes = one;
    planes.planes = true;
    gm20b::ComputeSpecialization a, b;
    a.bindTexture(2, &one);
    b.bindTexture(2, &planes);
    EXPECT_FALSE(a == b);
    EXPECT_EQ(a.text().textures, "2:550:2:1:1:02030405");
    EXPECT_EQ(b.text().textures, "2:550:2:1:1:02030405:xr2p");
    EXPECT_TRUE(a.bindTexture(2, &planes));
    EXPECT_EQ(a, b);
}

// The pack generator reads a key's textures back (pack/kernel_shapes.h): a two-plane texture's mark with them, anything
// else after the swizzle refused
TEST(GM20BComputeLookup, ATwoPlaneTexturesTextReadsBack)
{
    gm20b::BoundTexture t = {};
    ASSERT_TRUE(gm20b::parseComputeTexture("2:550:2:1:1:02030405:xr2p", t));
    EXPECT_TRUE(t.planes);
    EXPECT_EQ(gm20b::computeTextures(&t, 1), "2:550:2:1:1:02030405:xr2p");
    ASSERT_TRUE(gm20b::parseComputeTexture("3:80:2:1:1:00010203", t));
    EXPECT_FALSE(t.planes);
    EXPECT_EQ(gm20b::computeTextures(&t, 1), "3:80:2:1:1:00010203");
    EXPECT_FALSE(gm20b::parseComputeTexture("2:550:2:1:1:02030405:xr", t));
    EXPECT_FALSE(gm20b::parseComputeTexture("2:550:2:1:1", t));
}

// A converted texture's address map (Texture::addressMap) selects the kernel compiled under it, named by ":am<map>"
// after its slot's fields, and reads back for the compile; a texture of none keeps its key and text, byte for byte
TEST(GM20BComputeLookup, AConvertedTexturesAddressMapSelectsItsOwnKernelAndReadsBack)
{
    gm20b::BoundTexture none = {1, 502, 2, 1, 1, {2, 3, 4, 5}};
    gm20b::BoundTexture codes = none;
    codes.addressMap = 3;
    gm20b::ComputeSpecialization a, b;
    a.bindTexture(1, &none);
    b.bindTexture(1, &codes);
    EXPECT_FALSE(a == b);
    EXPECT_EQ(a.text().textures, "1:502:2:1:1:02030405");
    EXPECT_EQ(b.text().textures, "1:502:2:1:1:02030405:am3");
    gm20b::BoundTexture t = {};
    ASSERT_TRUE(gm20b::parseComputeTexture("1:502:2:1:1:02030405:am3", t));
    EXPECT_EQ(t.addressMap, 3);
    EXPECT_FALSE(t.planes);
    EXPECT_EQ(gm20b::computeTextures(&t, 1), "1:502:2:1:1:02030405:am3");
    EXPECT_FALSE(gm20b::parseComputeTexture("1:502:2:1:1:02030405:am", t));
    EXPECT_FALSE(gm20b::parseComputeTexture("1:502:2:1:1:02030405:am4", t));
    EXPECT_FALSE(gm20b::parseComputeTexture("1:502:2:1:1:02030405:am31", t));
}

TEST(GM20BComputeLookup, EverySpecializationInputSelectsAnotherKernel)
{
    gm20b::ComputeSpecialization key;
    key.threads = {8, 4, 1};
    auto t = texture(3);
    auto s = sampler(5);
    key.bindTexture(3, &t);
    key.bindSampler(5, &s);
    key.bindMemory(2, 48);
    std::vector<gm20b::ComputeSpecialization> changed;
    for (size_t axis = 0; axis < 3; ++axis) {
        auto next = key;
        ++next.threads[axis];
        changed.push_back(next);
    }
    for (unsigned field = 0; field < 8; ++field) {
        auto next = key;
        auto v = t;
        switch (field) {
        case 0:
            ++v.pixelFormat;
            break;
        case 1:
            ++v.textureType;
            break;
        case 2:
            ++v.levels;
            break;
        case 3:
            ++v.arrayLength;
            break;
        default:
            ++v.swizzle[field - 4];
            break;
        }
        next.bindTexture(3, &v);
        changed.push_back(next);
    }
    for (unsigned field = 0; field < 13; ++field) {
        auto next = key;
        auto v = s;
        switch (field) {
        case 0:
            ++v.minFilter;
            break;
        case 1:
            ++v.magFilter;
            break;
        case 2:
            ++v.mipFilter;
            break;
        case 3:
            ++v.sAddress;
            break;
        case 4:
            ++v.tAddress;
            break;
        case 5:
            ++v.rAddress;
            break;
        case 6:
            ++v.maxAnisotropy;
            break;
        case 7:
            ++v.compare;
            break;
        case 8:
            ++v.border;
            break;
        case 9:
            v.lodMin = -0.0f;
            break;
        case 10:
            ++v.lodMax;
            break;
        case 11:
            v.normalized = !v.normalized;
            break;
        case 12:
            v.lodAverage = !v.lodAverage;
            break;
        }
        next.bindSampler(5, &v);
        changed.push_back(next);
    }
    auto memory = key;
    memory.bindMemory(2, 64);
    changed.push_back(memory);
    gm20b::ComputeKernelCache<int> cache;
    int state = 1, calls = 0;
    const auto make = [&] {
        ++calls;
        return gm20b::ComputeKernelResult<int>{&state, false};
    };
    immediate(cache, key, make);
    for (const auto &next : changed) {
        EXPECT_FALSE(next == key);
        immediate(cache, next, make);
    }
    EXPECT_EQ(calls, 1 + changed.size());
}

TEST(GM20BComputeLookup, WarmLookupDoesNotInvokeLoader)
{
    gm20b::ComputeKernelCache<int> cache;
    gm20b::ComputeSpecialization key;
    int state = 1, calls = 0;
    for (int i = 0; i < 10000; ++i) {
        const auto result = immediate(cache, key, [&] {
            ++calls;
            return gm20b::ComputeKernelResult<int>{&state, false};
        });
        EXPECT_EQ(result.state, &state);
        EXPECT_FALSE(result.retry);
    }
    EXPECT_EQ(calls, 1);
}

TEST(GM20BComputeLookup, TransientFailureCanRecoverAndTerminalRefusalStaysCached)
{
    gm20b::ComputeKernelCache<int> cache;
    gm20b::ComputeSpecialization key;
    int state = 1, calls = 0;
    auto make = [&] {
        ++calls;
        return gm20b::ComputeKernelResult<int>{calls == 1 ? nullptr : &state, calls == 1};
    };
    EXPECT_TRUE(immediate(cache, key, make).retry);
    EXPECT_EQ(immediate(cache, key, make).state, &state);
    EXPECT_EQ(immediate(cache, key, make).state, &state);
    EXPECT_EQ(calls, 2);
    key.threads[0] = 7;
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(immediate(cache, key,
                            [&] {
                                ++calls;
                                return gm20b::ComputeKernelResult<int>{nullptr, false};
                            })
                      .state,
                  nullptr);
    EXPECT_EQ(calls, 3);
}

TEST(GM20BComputeLookup, PendingLookupReturnsImmediatelyAndOtherKeysProgress)
{
    gm20b::ComputeKernelCache<int> cache;
    gm20b::ComputeSpecialization key;
    int state = 1, other = 2;
    std::atomic<int> calls{0};
    auto first = cache.request(key, [&](auto) {
        ++calls;
    });
    auto second = std::async(std::launch::async, [&] {
        return cache.request(key, [&](auto) {
            ++calls;
        });
    });
    ASSERT_EQ(second.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(second.get(), first);
    EXPECT_EQ(first->state(), gm20b::ComputeKernelJob<int>::State::Pending);
    auto different = key;
    different.threads[0] = 2;
    EXPECT_EQ(immediate(cache, different,
                        [&] {
                            return gm20b::ComputeKernelResult<int>{&other, false};
                        })
                  .state,
              &other);
    int wakes = 0;
    first->observe([&] {
        ++wakes;
    });
    first->publish({&state, false});
    EXPECT_EQ(first->value(), &state);
    EXPECT_EQ(wakes, 1);
    EXPECT_EQ(calls.load(), 1);
    first->publish({nullptr, true});
    EXPECT_EQ(first->value(), &state);
    EXPECT_FALSE(first->retryable());
}

TEST(GM20BComputeLookup, RetryReplacementDoesNotInvalidateAnEarlierCommandBuffer)
{
    gm20b::ComputeKernelCache<int> cache;
    gm20b::ComputeSpecialization key;
    auto first = cache.request(key, [](auto job) {
        job->publish({nullptr, true});
    });
    auto retry = cache.request(key, [](auto) {});
    ASSERT_NE(first, retry);
    EXPECT_TRUE(first->retryable());
    EXPECT_EQ(first->state(), gm20b::ComputeKernelJob<int>::State::Failed);
    EXPECT_EQ(retry->state(), gm20b::ComputeKernelJob<int>::State::Pending);
    int value = 7;
    retry->publish({&value, false});
    EXPECT_EQ(retry->value(), &value);
    EXPECT_EQ(first->value(), nullptr);
}

TEST(GM20BComputeLookup, ObserverRegistrationRacingPublicationNeverLosesTheResult)
{
    for (unsigned iteration = 0; iteration < 200; ++iteration) {
        gm20b::ComputeKernelJob<int> job;
        int value = 9;
        std::atomic<unsigned> calls{0};
        std::thread producer([&] {
            job.publish({&value, false});
        });
        const auto observed = job.observe([&] {
            ++calls;
        });
        producer.join();
        EXPECT_TRUE(observed == gm20b::ComputeKernelJob<int>::State::Ready || calls == 1);
        EXPECT_LE(calls.load(), 1u);
        EXPECT_EQ(job.value(), &value);
    }
}

TEST(GM20BComputeLookup, UnwindingAJobDoesNotLeaveTheKeyPending)
{
    gm20b::ComputeKernelCache<int> cache;
    gm20b::ComputeSpecialization key;
    EXPECT_THROW(immediate(cache, key,
                           []() -> gm20b::ComputeKernelResult<int> {
                               throw std::runtime_error("compile");
                           }),
                 std::runtime_error);
    int state = 1;
    EXPECT_EQ(immediate(cache, key,
                        [&] {
                            return gm20b::ComputeKernelResult<int>{&state, false};
                        })
                  .state,
              &state);
}

TEST(GM20BComputeLookup, PlansEdgesAndRejectsOutOfRangeGroupsBeforeLookup)
{
    auto regular = gm20b::computeDispatch({64, 32, 1}, {8, 4, 1}, true);
    EXPECT_EQ(regular.reason, gm20b::ComputeDispatch::Ready);
    EXPECT_EQ(regular.groups, (std::array<uint64_t, 3>{8, 8, 1}));
    auto edge = gm20b::computeDispatch({65, 32, 1}, {8, 4, 1}, true);
    EXPECT_EQ(edge.reason, gm20b::ComputeDispatch::Ready);
    EXPECT_EQ(edge.count, 2);
    EXPECT_TRUE(edge.multi);
    EXPECT_EQ(edge.axis, 0);
    EXPECT_EQ(gm20b::computeDispatch({1, 1, 1}, {0, 1, 1}, true).reason, gm20b::ComputeDispatch::Edge);
    // past the QMD's grid fields: launched in slices (dispatch_bands.h qmdSlice), not refused
    EXPECT_EQ(gm20b::computeDispatch({1, 65536, 1}, {1, 1, 1}, false).reason, gm20b::ComputeDispatch::Ready);
    EXPECT_EQ(gm20b::computeDispatch({uint64_t(1) << 31, 1, 1}, {1, 1, 1}, false).reason,
              gm20b::ComputeDispatch::Ready);
    EXPECT_EQ(gm20b::computeDispatch({1, uint64_t(1) << 32, 1}, {1, 1, 1}, false).reason,
              gm20b::ComputeDispatch::Range);
    EXPECT_EQ(gm20b::computeDispatch({0, 0, 0}, {8, 4, 1}, true).reason, gm20b::ComputeDispatch::Ready);
}

}  // namespace

TEST(GM20BComputeLookup, RegionsCoverEveryThreadExactlyOnceWithTrueBuiltinCoordinates)
{
    for (const auto grid : {std::array<uint64_t, 3>{19, 11, 7}, {3, 2, 1}, {16, 10, 6}, {16, 11, 6}}) {
        const auto plan = gm20b::computeDispatch(grid, {8, 5, 3}, true);
        ASSERT_EQ(plan.reason, gm20b::ComputeDispatch::Ready);
        std::vector<uint8_t> visits(grid[0] * grid[1] * grid[2]);
        for (uint8_t i = 0; i < plan.count; ++i) {
            const auto &r = plan.regions[i];
            uint32_t words[8];
            ASSERT_TRUE(gm20b::computeRegionWords(r.base, plan.grid, plan.nominal, words));
            for (uint32_t axis = 0; axis < 3; ++axis) {
                EXPECT_EQ(words[axis], r.base[axis]);
                EXPECT_EQ(words[axis + 4], grid[axis]);
                EXPECT_EQ(((words[3] >> (10 * axis)) & 1023) + 1, plan.nominal[axis]);
            }
            for (uint32_t z = 0; z < r.groups[2] * r.local[2]; ++z)
                for (uint32_t y = 0; y < r.groups[1] * r.local[1]; ++y)
                    for (uint32_t x = 0; x < r.groups[0] * r.local[0]; ++x) {
                        const uint64_t gx = r.base[0] * plan.nominal[0] + x;
                        const uint64_t gy = r.base[1] * plan.nominal[1] + y;
                        const uint64_t gz = r.base[2] * plan.nominal[2] + z;
                        ASSERT_LT(gx, grid[0]);
                        ASSERT_LT(gy, grid[1]);
                        ASSERT_LT(gz, grid[2]);
                        ++visits[(gz * grid[1] + gy) * grid[0] + gx];
                    }
        }
        for (auto count : visits)
            EXPECT_EQ(count, 1);
    }
}

TEST(GM20BComputeLookup, RegionSpecializationDoesNotAliasUniformKernelAndReusesWarmJobs)
{
    gm20b::ComputeKernelCache<int> cache;
    gm20b::ComputeSpecialization uniform;
    uniform.threads = {3, 2, 1};
    auto region = uniform;
    region.regionMode = true;
    unsigned made = 0;
    int value = 1;
    auto start = [&](auto job) {
        ++made;
        job->publish({&value, false});
    };
    auto a = cache.request(uniform, start), b = cache.request(region, start);
    EXPECT_NE(a, b);
    EXPECT_EQ(cache.request(region, start), b);
    EXPECT_EQ(made, 2u);
    gm20b::AirHash hash{};
    EXPECT_NE(gm20b::computeKey(hash, "", 0, false, uniform.threads.data(), "", "", ""),
              gm20b::computeKey(hash, "", 0, false, region.threads.data(), "", "", "", true));
}

TEST(GM20BComputeLookup, RegionSetCannotAdmitPartialCompilationOrEraseRefusal)
{
    using Set = gm20b::ComputeKernelSet<int>;
    Set group;
    group.count = 2;
    for (unsigned i = 0; i < 2; ++i)
        group.jobs[i] = std::make_shared<Set::Job>();
    int first = 1;
    unsigned wakes = 0;
    EXPECT_EQ(group.observe([&] {
        ++wakes;
    }),
              Set::State::Pending);
    group.jobs[0]->publish({&first, false});
    EXPECT_EQ(group.state(), Set::State::Pending);
    group.jobs[1]->publish({nullptr, true});
    EXPECT_EQ(group.state(), Set::State::Failed);
    EXPECT_TRUE(group.retryable());
    EXPECT_EQ(wakes, 2u);
}

TEST(GM20BComputeLookup, FullGridArithmeticNeverTruncatesOrWraps)
{
    EXPECT_EQ(gm20b::computeDispatch({UINT64_MAX, 1, 1}, {8, 1, 1}, false).reason, gm20b::ComputeDispatch::Range);
    EXPECT_EQ(gm20b::computeDispatch({1u << 30, 1, 1}, {8, 1, 1}, false).reason, gm20b::ComputeDispatch::Range);
    EXPECT_EQ(gm20b::computeDispatch({0, 5, 3}, {8, 5, 3}, true).count, 0);
}

TEST(GM20BComputeLookup, SurfaceAliasesUseEveryRegionResourceEvenWhenSpecializationEliminatesOtherSlots)
{
    gm20b::PackKernel interior{}, edge{};
    interior.entries = 1;
    interior.entry[0].kind = gm20b::KernelTexture;
    interior.entry[0].slot = 7;
    edge.entries = 2;
    edge.entry[0].kind = gm20b::KernelBuffer;
    edge.entry[0].slot = 2;
    edge.entry[1].kind = gm20b::KernelTexture;
    edge.entry[1].slot = 4;
    gm20b::ComputeResourceSlots used;
    used.add(interior);
    auto texture = [](uint32_t slot) {
        return slot == 4;
    };
    auto buffer = [](uint32_t slot) {
        return slot == 2;
    };
    auto unrelated = [](uint32_t) {
        return false;
    };
    EXPECT_FALSE(used.aliases(texture, buffer));
    used.add(edge);
    EXPECT_TRUE(used.aliases(texture, unrelated));
    EXPECT_TRUE(used.aliases(unrelated, buffer));
    EXPECT_FALSE(used.aliases(unrelated, unrelated));
}

TEST(GM20BComputeLookup, InvalidNominalThreadCountIsRefusedBeforePlanningEvenWhenTailWouldFit)
{
    EXPECT_EQ(gm20b::computeDispatch({1, 1, 1}, {1024, 2, 1}, true).reason, gm20b::ComputeDispatch::Edge);
    EXPECT_EQ(gm20b::computeDispatch({1, 1, 1}, {32, 32, 2}, true).reason, gm20b::ComputeDispatch::Edge);
    EXPECT_EQ(gm20b::computeDispatch({1, 1, 1}, {16, 16, 1}, true, 128).reason, gm20b::ComputeDispatch::Edge);
    EXPECT_EQ(gm20b::computeDispatch({1, 1, 1}, {8, 16, 1}, true, 128).reason, gm20b::ComputeDispatch::Ready);
    EXPECT_EQ(gm20b::computeDispatch({1, 1, 1}, {1, 1, 1}, true, 0).reason, gm20b::ComputeDispatch::Edge);
}

// A build that failed (its compile could not run, or no room to place it) fails that make alone: the next make tries
// afresh at once, with no delay and no count, and keeps the record it already made
TEST(GM20BComputeLookup, AFailedBuildTriesAfreshAtTheNextMakeAndKeepsItsRecord)
{
    using Build = gm20b::ComputeKernelBuild<int, int>;
    Build build;
    unsigned produces = 0, places = 0;
    int state = 7;
    const auto produce = [&](int &record) {
        record = 42;
        ++produces;
        return produces < 8 ? Build::Outcome::Failed : Build::Outcome::Made;
    };
    const auto place = [&](const int &record) {
        ++places;
        EXPECT_EQ(record, 42);
        return &state;
    };
    for (unsigned i = 0; i < 7; ++i) {
        const auto failed = build.make(produce, place);
        EXPECT_EQ(failed.state, nullptr);
        EXPECT_TRUE(failed.retry);
        EXPECT_EQ(produces, i + 1);
    }
    EXPECT_EQ(build.make(produce, place).state, &state);
    EXPECT_EQ(produces, 8u);
    EXPECT_EQ(places, 1u);
    for (unsigned i = 0; i < 10000; ++i)
        EXPECT_EQ(build.make(produce, place).state, &state);
    EXPECT_EQ(produces, 8u);
    EXPECT_EQ(places, 1u);

    Build unplaced;
    unsigned tries = 0;
    const auto made = [&](int &record) {
        record = 42;
        return Build::Outcome::Made;
    };
    const auto room = [&](const int &) -> const int * {
        return ++tries < 4 ? nullptr : &state;
    };
    for (unsigned i = 0; i < 3; ++i)
        EXPECT_TRUE(unplaced.make(made, room).retry);
    EXPECT_EQ(unplaced.make(made, room).state, &state);
    EXPECT_EQ(tries, 4u);
}

TEST(GM20BComputeLookup, SemanticBuildRefusalNeverRetriesOrAttemptsPlacement)
{
    using Build = gm20b::ComputeKernelBuild<int, int>;
    Build build;
    unsigned produces = 0, places = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const auto result = build.make(
            [&](int &) {
                ++produces;
                return Build::Outcome::Refused;
            },
            [&](const int &) -> const int * {
                ++places;
                return nullptr;
            });
        EXPECT_EQ(result.state, nullptr);
        EXPECT_FALSE(result.retry);
    }
    EXPECT_EQ(produces, 1u);
    EXPECT_EQ(places, 0u);
}

// A failed job fails the command buffer that needed it; the next dispatch of its key starts another job at once
TEST(GM20BComputeLookup, AFailedJobFailsItsDispatchAndTheNextDispatchStartsAnother)
{
    gm20b::ComputeKernelCache<int> cache;
    gm20b::ComputeSpecialization key;
    unsigned starts = 0;
    auto start = [&](auto job) {
        ++starts;
        job->publish({nullptr, true});
    };
    auto failed = cache.request(key, start);
    EXPECT_EQ(failed->state(), gm20b::ComputeKernelJob<int>::State::Failed);
    EXPECT_TRUE(failed->retryable());
    auto next = cache.request(key, start);
    EXPECT_NE(next, failed);
    EXPECT_EQ(starts, 2u);
    int value = 3;
    auto made = cache.request(key, [&](auto job) {
        job->publish({&value, false});
    });
    EXPECT_EQ(made->value(), &value);
    EXPECT_EQ(cache.request(key, start), made);
    EXPECT_EQ(starts, 2u);
}
