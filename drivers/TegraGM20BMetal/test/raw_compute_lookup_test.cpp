// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_compute_lookup.h"
#include "../raw/runtime/raw_compute_record.h"
#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

namespace {

template <typename Make>
raw::ComputeKernelResult<int> immediate(raw::ComputeKernelCache<int> &cache, const raw::ComputeSpecialization &key,
                                        Make make)
{
    auto job = cache.request(key, [&](auto job) {
        job->publish(make());
    });
    return {job->value(), job->retryable()};
}

raw::BoundTexture texture(uint32_t slot = 0)
{
    return {slot, 80, 2, 1, 1, {0, 1, 2, 3}};
}

raw::BoundSampler sampler(uint32_t slot = 0)
{
    return {slot, 1, 1, 0, 0, 0, 0, 1, 7, 0, 0.0f, 16.0f, true, false};
}

TEST(RawComputeLookup, BindingOrderAndClearedSlotsHaveCanonicalKeys)
{
    raw::ComputeSpecialization a, b;
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

TEST(RawComputeLookup, TextPreservesPackAndJournalIdentity)
{
    raw::ComputeSpecialization key;
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

TEST(RawComputeLookup, EverySpecializationInputSelectsAnotherKernel)
{
    raw::ComputeSpecialization key;
    key.threads = {8, 4, 1};
    auto t = texture(3);
    auto s = sampler(5);
    key.bindTexture(3, &t);
    key.bindSampler(5, &s);
    key.bindMemory(2, 48);
    std::vector<raw::ComputeSpecialization> changed;
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
    raw::ComputeKernelCache<int> cache;
    int state = 1, calls = 0;
    const auto make = [&] {
        ++calls;
        return raw::ComputeKernelResult<int>{&state, false};
    };
    immediate(cache, key, make);
    for (const auto &next : changed) {
        EXPECT_FALSE(next == key);
        immediate(cache, next, make);
    }
    EXPECT_EQ(calls, 1 + changed.size());
}

TEST(RawComputeLookup, WarmLookupDoesNotInvokeLoader)
{
    raw::ComputeKernelCache<int> cache;
    raw::ComputeSpecialization key;
    int state = 1, calls = 0;
    for (int i = 0; i < 10000; ++i) {
        const auto result = immediate(cache, key, [&] {
            ++calls;
            return raw::ComputeKernelResult<int>{&state, false};
        });
        EXPECT_EQ(result.state, &state);
        EXPECT_FALSE(result.retry);
    }
    EXPECT_EQ(calls, 1);
}

TEST(RawComputeLookup, TransientFailureCanRecoverAndTerminalRefusalStaysCached)
{
    raw::ComputeKernelCache<int> cache;
    raw::ComputeSpecialization key;
    int state = 1, calls = 0;
    auto make = [&] {
        ++calls;
        return raw::ComputeKernelResult<int>{calls == 1 ? nullptr : &state, calls == 1};
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
                                return raw::ComputeKernelResult<int>{nullptr, false};
                            })
                      .state,
                  nullptr);
    EXPECT_EQ(calls, 3);
}

TEST(RawComputeLookup, PendingLookupReturnsImmediatelyAndOtherKeysProgress)
{
    raw::ComputeKernelCache<int> cache;
    raw::ComputeSpecialization key;
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
    EXPECT_EQ(first->state(), raw::ComputeKernelJob<int>::State::Pending);
    auto different = key;
    different.threads[0] = 2;
    EXPECT_EQ(immediate(cache, different,
                        [&] {
                            return raw::ComputeKernelResult<int>{&other, false};
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

TEST(RawComputeLookup, RetryReplacementDoesNotInvalidateAnEarlierCommandBuffer)
{
    raw::ComputeKernelCache<int> cache;
    raw::ComputeSpecialization key;
    auto first = cache.request(key, [](auto job) {
        job->publish({nullptr, true});
    });
    auto retry = cache.request(key, [](auto) {});
    ASSERT_NE(first, retry);
    EXPECT_TRUE(first->retryable());
    EXPECT_EQ(first->state(), raw::ComputeKernelJob<int>::State::Refused);
    EXPECT_EQ(retry->state(), raw::ComputeKernelJob<int>::State::Pending);
    int value = 7;
    retry->publish({&value, false});
    EXPECT_EQ(retry->value(), &value);
    EXPECT_EQ(first->value(), nullptr);
}

TEST(RawComputeLookup, ObserverRegistrationRacingPublicationNeverLosesTheResult)
{
    for (unsigned iteration = 0; iteration < 200; ++iteration) {
        raw::ComputeKernelJob<int> job;
        int value = 9;
        std::atomic<unsigned> calls{0};
        std::thread producer([&] {
            job.publish({&value, false});
        });
        const auto observed = job.observe([&] {
            ++calls;
        });
        producer.join();
        EXPECT_TRUE(observed == raw::ComputeKernelJob<int>::State::Ready || calls == 1);
        EXPECT_LE(calls.load(), 1u);
        EXPECT_EQ(job.value(), &value);
    }
}

TEST(RawComputeLookup, UnwindingAJobDoesNotLeaveTheKeyPending)
{
    raw::ComputeKernelCache<int> cache;
    raw::ComputeSpecialization key;
    EXPECT_THROW(immediate(cache, key,
                           []() -> raw::ComputeKernelResult<int> {
                               throw std::runtime_error("compile");
                           }),
                 std::runtime_error);
    int state = 1;
    EXPECT_EQ(immediate(cache, key,
                        [&] {
                            return raw::ComputeKernelResult<int>{&state, false};
                        })
                  .state,
              &state);
}

TEST(RawComputeLookup, PlansEdgesAndRejectsOutOfRangeGroupsBeforeLookup)
{
    auto regular = raw::computeDispatch({64, 32, 1}, {8, 4, 1}, true);
    EXPECT_EQ(regular.reason, raw::ComputeDispatch::Ready);
    EXPECT_EQ(regular.groups, (std::array<uint64_t, 3>{8, 8, 1}));
    auto edge = raw::computeDispatch({65, 32, 1}, {8, 4, 1}, true);
    EXPECT_EQ(edge.reason, raw::ComputeDispatch::Ready);
    EXPECT_EQ(edge.count, 2);
    EXPECT_TRUE(edge.multi);
    EXPECT_EQ(edge.axis, 0);
    EXPECT_EQ(raw::computeDispatch({1, 1, 1}, {0, 1, 1}, true).reason, raw::ComputeDispatch::Edge);
    EXPECT_EQ(raw::computeDispatch({1, 65536, 1}, {1, 1, 1}, false).reason, raw::ComputeDispatch::Range);
    EXPECT_EQ(raw::computeDispatch({uint64_t(1) << 31, 1, 1}, {1, 1, 1}, false).reason, raw::ComputeDispatch::Range);
    EXPECT_EQ(raw::computeDispatch({0, 0, 0}, {8, 4, 1}, true).reason, raw::ComputeDispatch::Ready);
}

}  // namespace

TEST(RawComputeLookup, RegionsCoverEveryThreadExactlyOnceWithTrueBuiltinCoordinates)
{
    for (const auto grid : {std::array<uint64_t, 3>{19, 11, 7}, {3, 2, 1}, {16, 10, 6}, {16, 11, 6}}) {
        const auto plan = raw::computeDispatch(grid, {8, 5, 3}, true);
        ASSERT_EQ(plan.reason, raw::ComputeDispatch::Ready);
        std::vector<uint8_t> visits(grid[0] * grid[1] * grid[2]);
        for (uint8_t i = 0; i < plan.count; ++i) {
            const auto &r = plan.regions[i];
            uint32_t words[8];
            ASSERT_TRUE(raw::computeRegionWords(r.base, plan.grid, plan.nominal, words));
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

TEST(RawComputeLookup, RegionSpecializationDoesNotAliasUniformKernelAndReusesWarmJobs)
{
    raw::ComputeKernelCache<int> cache;
    raw::ComputeSpecialization uniform;
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
    raw::AirHash hash{};
    EXPECT_NE(raw::computeKey(hash, "", 0, false, uniform.threads.data(), "", "", ""),
              raw::computeKey(hash, "", 0, false, region.threads.data(), "", "", "", true));
}

TEST(RawComputeLookup, RegionSetCannotAdmitPartialCompilationOrEraseRefusal)
{
    using Set = raw::ComputeKernelSet<int>;
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
    EXPECT_EQ(group.state(), Set::State::Refused);
    EXPECT_TRUE(group.retryable());
    EXPECT_EQ(wakes, 2u);
}

TEST(RawComputeLookup, FullGridArithmeticNeverTruncatesOrWraps)
{
    EXPECT_EQ(raw::computeDispatch({UINT64_MAX, 1, 1}, {8, 1, 1}, false).reason, raw::ComputeDispatch::Range);
    EXPECT_EQ(raw::computeDispatch({1u << 30, 1, 1}, {8, 1, 1}, false).reason, raw::ComputeDispatch::Range);
    EXPECT_EQ(raw::computeDispatch({0, 5, 3}, {8, 5, 3}, true).count, 0);
}

TEST(RawComputeLookup, SurfaceAliasesUseEveryRegionResourceEvenWhenSpecializationEliminatesOtherSlots)
{
    raw::PackKernel interior{}, edge{};
    interior.entries = 1;
    interior.entry[0].kind = raw::KernelTexture;
    interior.entry[0].slot = 7;
    edge.entries = 2;
    edge.entry[0].kind = raw::KernelBuffer;
    edge.entry[0].slot = 2;
    edge.entry[1].kind = raw::KernelTexture;
    edge.entry[1].slot = 4;
    raw::ComputeResourceSlots used;
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

TEST(RawComputeLookup, InvalidNominalThreadCountIsRefusedBeforePlanningEvenWhenTailWouldFit)
{
    EXPECT_EQ(raw::computeDispatch({1, 1, 1}, {1024, 2, 1}, true).reason, raw::ComputeDispatch::Edge);
    EXPECT_EQ(raw::computeDispatch({1, 1, 1}, {32, 32, 2}, true).reason, raw::ComputeDispatch::Edge);
    EXPECT_EQ(raw::computeDispatch({1, 1, 1}, {16, 16, 1}, true, 128).reason, raw::ComputeDispatch::Edge);
    EXPECT_EQ(raw::computeDispatch({1, 1, 1}, {8, 16, 1}, true, 128).reason, raw::ComputeDispatch::Ready);
    EXPECT_EQ(raw::computeDispatch({1, 1, 1}, {1, 1, 1}, true, 0).reason, raw::ComputeDispatch::Edge);
}

TEST(RawComputeLookup, TransientBuildFailuresStayRetryableBeyondThreeAttempts)
{
    using Build = raw::ComputeKernelBuild<int, int>;
    Build build;
    uint64_t now = 100, clocks = 0;
    unsigned produces = 0, places = 0;
    int state = 7;
    const auto clock = [&] {
        ++clocks;
        return now;
    };
    const auto produce = [&](int &record) {
        record = 42;
        ++produces;
        now += 2000000;
        return produces < 8 ? Build::Outcome::Failed : Build::Outcome::Made;
    };
    const auto place = [&](const int &record) {
        ++places;
        EXPECT_EQ(record, 42);
        return &state;
    };
    for (unsigned i = 0; i < 7; ++i) {
        const auto failed = build.make(clock, produce, place);
        EXPECT_EQ(failed.state, nullptr);
        ASSERT_TRUE(failed.retry);
        EXPECT_GT(failed.retryAtUs, now);
        EXPECT_LE(failed.retryAtUs - now, 1000000u);
        const auto stillWaiting = build.make(clock, produce, place);
        EXPECT_TRUE(stillWaiting.retry);
        EXPECT_EQ(produces, i + 1);
        now = failed.retryAtUs;
    }
    EXPECT_EQ(build.make(clock, produce, place).state, &state);
    EXPECT_EQ(produces, 8u);
    EXPECT_EQ(places, 1u);
    const auto before = clocks;
    for (unsigned i = 0; i < 10000; ++i)
        EXPECT_EQ(build.make(clock, produce, place).state, &state);
    EXPECT_EQ(clocks, before);
    EXPECT_EQ(produces, 8u);
    EXPECT_EQ(places, 1u);
}

TEST(RawComputeLookup, SemanticBuildRefusalNeverRetriesOrAttemptsPlacement)
{
    using Build = raw::ComputeKernelBuild<int, int>;
    Build build;
    unsigned produces = 0, places = 0, clocks = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const auto result = build.make(
            [&] {
                ++clocks;
                return UINT64_MAX;
            },
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
        EXPECT_EQ(result.retryAtUs, 0u);
    }
    EXPECT_EQ(produces, 1u);
    EXPECT_EQ(places, 0u);
    EXPECT_EQ(clocks, 0u);
}

TEST(RawComputeLookup, RetryNotificationPreventsWarmDispatchFromStartingRepeatedFailedJobs)
{
    raw::ComputeKernelCache<int> cache;
    raw::ComputeSpecialization key;
    unsigned starts = 0;
    auto start = [&](auto job) {
        ++starts;
        job->publish({nullptr, true, true});
    };
    auto failed = cache.request(key, start);
    for (unsigned i = 0; i < 10000; ++i) {
        EXPECT_EQ(cache.request(key, start), failed);
        EXPECT_FALSE(failed->retryable());
    }
    EXPECT_EQ(starts, 1u);
    failed->allowRetry();
    auto next = cache.request(key, start);
    EXPECT_NE(next, failed);
    EXPECT_EQ(starts, 2u);
    EXPECT_FALSE(next->retryable());
}

TEST(RawComputeLookup, RetryNotificationBeforeFailurePublicationIsNotLost)
{
    raw::ComputeKernelJob<int> job;
    job.allowRetry();
    EXPECT_FALSE(job.retryable());
    job.publish({nullptr, true, true});
    EXPECT_TRUE(job.retryable());
}
