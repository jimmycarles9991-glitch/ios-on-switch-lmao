// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <vector>
#include "../raw/runtime/raw_replay_twins.h"

namespace {

struct Object {
    bool twin;
};

// RawMetalReplay.mm's shape: a range's passes, each with its attachments (null where it has none), encoded only once every
// attachment of every pass has a twin
struct Range {
    std::vector<std::vector<Object *>> passes;
    size_t visited = 0, encoded = 0;

    void *missing()
    {
        return raw::missingTwin(
            0, passes.size(),
            [this](size_t at, auto &&visit) {
                for (Object *o : passes[at]) {
                    ++visited;
                    visit(o);
                }
            },
            [](void *o) {
                return static_cast<Object *>(o)->twin;
            });
    }

    bool replay()
    {
        if (missing())
            return false;
        encoded = passes.size();
        return true;
    }
};

TEST(ReplayTwins, EveryAttachmentWithATwinReplays)
{
    Object color{true}, depth{true};
    Range r{{{&color, nullptr, &depth}, {nullptr}, {&color}}};
    EXPECT_EQ(r.missing(), nullptr);
    EXPECT_TRUE(r.replay());
    EXPECT_EQ(r.encoded, 3u);
}

TEST(ReplayTwins, ALaterPassMissingATwinFailsTheRangeBeforeAnyPassIsEncoded)
{
    Object color{true}, heapTarget{false};
    Range r{{{&color}, {&color, &heapTarget}, {&color}}};
    EXPECT_FALSE(r.replay());
    EXPECT_EQ(r.encoded, 0u);
    EXPECT_EQ(r.missing(), &heapTarget);
}

TEST(ReplayTwins, TheFirstMissingAttachmentIsNamedAndTheRestNotRead)
{
    Object a{false}, b{false};
    Range r{{{nullptr, &a}, {&b}}};
    EXPECT_EQ(r.missing(), &a);
    EXPECT_EQ(r.visited, 2u);
}

struct Use {
    raw::ReplayUse kind;
};

struct State {
    bool readsArguments;
};

// RawMetalReplay.mm's rawArgumentsIn over a range's uses and the states it sets
void *rawArguments(const std::vector<Use *> &uses, const std::vector<State *> &states)
{
    return raw::rawArgumentsIn(
        [&](auto &&visit) {
            for (Use *u : uses)
                visit(u);
        },
        [](void *u) {
            return static_cast<Use *>(u)->kind;
        },
        [&](auto &&visit) {
            for (State *s : states)
                visit(s);
        },
        [](void *s) {
            return static_cast<State *>(s)->readsArguments;
        });
}

TEST(ReplayTwins, AHeapWhoseResourcesTheRangeBindsReplays)
{
    Use heap{raw::ReplayUse::Heap}, texture{raw::ReplayUse::Other};
    State slots{false};
    EXPECT_EQ(rawArguments({&heap, &texture}, {&slots}), nullptr);
    EXPECT_EQ(rawArguments({&texture}, {}), nullptr);
}

TEST(ReplayTwins, AHeapBesideAStateThatReadsArgumentBuffersFailsTheRange)
{
    Use heap{raw::ReplayUse::Heap};
    State slots{false}, arguments{true};
    EXPECT_EQ(rawArguments({&heap}, {&slots, &arguments}), &heap);
}

TEST(ReplayTwins, ABufferAnArgumentEncoderWroteFailsTheRangeWhateverItsStates)
{
    Use heap{raw::ReplayUse::Heap}, written{raw::ReplayUse::WrittenArguments};
    State arguments{true};
    EXPECT_EQ(rawArguments({&heap, &written}, {}), &written);
    EXPECT_EQ(rawArguments({&written}, {&arguments}), &written);
}

}  // namespace
