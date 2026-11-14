// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <map>
#include <vector>
#include "../SwitchVICGate.cpp"

namespace {
struct Submitted {
    TegraVICPlan plan;
    TegraVICAcquire acquire;
    void (*completion)(void *, int);
    void *context;
};

std::map<uint32_t, TegraVICSurfaceDesc> surfaces;
std::vector<Submitted> submissions;
int transformResult = 1, held = 0, written = 0, allocations = 0, increments = 0, incrementError = 0;
uint32_t syncptValue = 40;
task_t lookedUpIn = nullptr;
}  // namespace

const TegraVICTraits *tegra_vic_traits()
{
    return &kTegraVICTraits;
}

bool tegra_vic_ready()
{
    return true;
}

IOReturn tegra_vic_surface_lookup(uint32_t id, task_t task, TegraVICSurface *out)
{
    lookedUpIn = task;
    const auto found = surfaces.find(id);
    if (found == surfaces.end())
        return kIOReturnBadArgument;
    *out = {};
    out->surface = reinterpret_cast<OSObject *>(uintptr_t(id));
    out->desc = found->second;
    out->id = id;
    ++held;
    return kIOReturnSuccess;
}

void tegra_vic_surface_release(TegraVICSurface *s)
{
    if (s->surface)
        --held;
    *s = {};
}

void tegra_vic_surface_written(const TegraVICSurface *)
{
    ++written;
}

int switch_vic_transform(const TegraVICSurface *source, const TegraVICSurface *destination, const TegraVICPlan *plan,
                         uint32_t chromaSite, void (*completion)(void *, int), void *context, SwitchVICEvents *events,
                         const TegraVICAcquire *acquire)
{
    EXPECT_EQ(source->desc.fourcc, destination->desc.fourcc);
    EXPECT_EQ(chromaSite, 0u);
    EXPECT_EQ(events, nullptr);
    if (transformResult == 1)
        submissions.push_back({*plan, acquire ? *acquire : TegraVICAcquire{}, completion, context});
    return transformResult;
}

int tegra_host1x_syncpt_alloc(uint32_t *id)
{
    ++allocations;
    *id = 175;
    return 0;
}

uint32_t tegra_host1x_syncpt_read(uint32_t id)
{
    EXPECT_EQ(id, 175u);
    return syncptValue;
}

int tegra_host1x_syncpt_cpu_increment(uint32_t id)
{
    EXPECT_EQ(id, 175u);
    ++increments;
    if (incrementError)
        return incrementError;
    ++syncptValue;
    return 0;
}

namespace {
constexpr uint32_t kSource = 0x51, kDestination = 0x52, kOdd = 0x53, kSmall = 0x54;
const void *const kTest = reinterpret_cast<const void *>(0xc0);
const void *const kOther = reinterpret_cast<const void *>(0xc1);

TegraVICSurfaceDesc nv12(uint32_t w, uint32_t h, uint32_t pitch)
{
    TegraVICSurfaceDesc d = {};
    d.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
    d.width = w;
    d.height = h;
    d.planes = 2;
    d.planeBytesPerRow[0] = d.planeBytesPerRow[1] = pitch;
    d.planeHeight[0] = h;
    d.planeHeight[1] = h / 2;
    d.planeOffset[1] = uint64_t(pitch) * h;
    d.length = uint64_t(pitch) * h * 3 / 2;
    return d;
}

class SwitchVICGateTest : public testing::Test {
protected:
    void SetUp() override
    {
        surfaces = {{kSource, nv12(1280, 720, 1280)},
                    {kDestination, nv12(1280, 720, 1280)},
                    {kOdd, nv12(1280, 720, 1344)},
                    {kSmall, nv12(640, 360, 768)}};
        submissions.clear();
        transformResult = 1;
        written = increments = incrementError = 0;
    }

    void TearDown() override
    {
        for (const auto &s : submissions)
            if (!__atomic_load_n(&gGate.done, __ATOMIC_ACQUIRE))
                s.completion(s.context, 0);
        switch_vic_gate_close(kTest);
        switch_vic_gate_close(kOther);
        EXPECT_EQ(held, 0);
    }

    static SwitchVICGateReply call(uint32_t op, const void *client = kTest, uint32_t source = kSource,
                                   uint32_t destination = kDestination, IOReturn *result = nullptr)
    {
        const SwitchVICGateRequest request = {SWITCH_VIC_GATE_MAGIC, op, source, destination};
        SwitchVICGateReply reply = {};
        IOExternalMethodArguments a;
        a.structureInput = &request;
        a.structureInputSize = sizeof(request);
        a.structureOutput = &reply;
        a.structureOutputSize = sizeof(reply);
        const IOReturn returned = switch_vic_gate_call(client, &a);
        if (result)
            *result = returned;
        else
            EXPECT_EQ(returned, kIOReturnSuccess);
        return reply;
    }
};

// The copy is queued with one wait first: the gate one past where it is now, so only the release lets VIC run it.
TEST_F(SwitchVICGateTest, ArmQueuesTheWholeCopyBehindTheGate)
{
    syncptValue = 40;
    const auto reply = call(SWITCH_VIC_GATE_ARM);
    EXPECT_EQ(reply.error, 0);
    EXPECT_EQ(reply.state, uint32_t(SWITCH_VIC_GATE_HELD));
    ASSERT_EQ(submissions.size(), 1u);
    const auto &s = submissions[0];
    EXPECT_EQ(s.acquire.syncpt, 175u);
    EXPECT_EQ(s.acquire.threshold, 41u);
    EXPECT_EQ(reply.threshold, 41u);
    EXPECT_EQ(s.plan.count, 1u);
    EXPECT_EQ(s.plan.readsSource, 1u);
    EXPECT_EQ(s.plan.op[0].srcW, uint64_t(1280) << 16);
    EXPECT_EQ(s.plan.op[0].srcH, uint64_t(720) << 16);
    EXPECT_EQ(s.plan.op[0].dstW, 1280u);
    EXPECT_EQ(s.plan.op[0].dstH, 720u);
    EXPECT_EQ(s.plan.op[0].transform, 0u);
    EXPECT_EQ(lookedUpIn, gateCallerTask);
    EXPECT_EQ(held, 2);
    EXPECT_EQ(increments, 0);
}

TEST_F(SwitchVICGateTest, ReleaseMovesTheGateOnceAndTheCompletionEndsIt)
{
    call(SWITCH_VIC_GATE_ARM);
    EXPECT_EQ(call(SWITCH_VIC_GATE_RELEASE).state, uint32_t(SWITCH_VIC_GATE_RELEASED));
    EXPECT_EQ(increments, 1);
    EXPECT_EQ(call(SWITCH_VIC_GATE_RELEASE).error, 22) << "released already";
    EXPECT_EQ(increments, 1);
    submissions[0].completion(submissions[0].context, 0);
    const auto done = call(SWITCH_VIC_GATE_QUERY);
    EXPECT_EQ(done.state, uint32_t(SWITCH_VIC_GATE_DONE));
    EXPECT_EQ(done.status, 0);
    EXPECT_EQ(written, 1);
    EXPECT_EQ(held, 0);
}

TEST_F(SwitchVICGateTest, AFailedCopyIsReportedAsItsStatus)
{
    call(SWITCH_VIC_GATE_ARM);
    call(SWITCH_VIC_GATE_RELEASE);
    submissions[0].completion(submissions[0].context, -5);
    EXPECT_EQ(call(SWITCH_VIC_GATE_QUERY).status, -5);
}

TEST_F(SwitchVICGateTest, OneGateAtATimeUntilItsCopyIsDone)
{
    call(SWITCH_VIC_GATE_ARM);
    EXPECT_EQ(call(SWITCH_VIC_GATE_ARM).error, 16);
    EXPECT_EQ(submissions.size(), 1u);
    call(SWITCH_VIC_GATE_RELEASE);
    EXPECT_EQ(call(SWITCH_VIC_GATE_ARM).error, 16) << "released, not yet done";
    submissions[0].completion(submissions[0].context, 0);
    syncptValue = 77;
    const auto again = call(SWITCH_VIC_GATE_ARM);
    EXPECT_EQ(again.error, 0);
    ASSERT_EQ(submissions.size(), 2u);
    EXPECT_EQ(submissions[1].acquire.threshold, 78u);
    EXPECT_EQ(allocations, 1) << "the gate's syncpoint is made once";
}

TEST_F(SwitchVICGateTest, OnlyTheArmingConnectionReleases)
{
    call(SWITCH_VIC_GATE_ARM);
    EXPECT_EQ(call(SWITCH_VIC_GATE_RELEASE, kOther).error, 22);
    switch_vic_gate_close(kOther);
    EXPECT_EQ(increments, 0);
    EXPECT_EQ(call(SWITCH_VIC_GATE_QUERY, kOther).state, uint32_t(SWITCH_VIC_GATE_HELD));
}

// A test that dies holding its gate must not leave VIC's channel waiting on it.
TEST_F(SwitchVICGateTest, ClosingTheConnectionReleasesItsHeldGate)
{
    call(SWITCH_VIC_GATE_ARM);
    switch_vic_gate_close(kTest);
    EXPECT_EQ(increments, 1);
    EXPECT_EQ(call(SWITCH_VIC_GATE_QUERY).state, uint32_t(SWITCH_VIC_GATE_RELEASED));
    switch_vic_gate_close(kTest);
    EXPECT_EQ(increments, 1);
}

// A gate that does not move is one line and that release fails; it is not written again.
TEST_F(SwitchVICGateTest, AGateThatDoesNotMoveFailsThatReleaseWithOneLine)
{
    call(SWITCH_VIC_GATE_ARM);
    incrementError = -5;
    const int lines = gateLogLines;
    const auto reply = call(SWITCH_VIC_GATE_RELEASE);
    EXPECT_EQ(reply.error, 5);
    EXPECT_EQ(reply.state, uint32_t(SWITCH_VIC_GATE_HELD));
    EXPECT_EQ(increments, 1);
    EXPECT_EQ(gateLogLines, lines + 1);
    incrementError = 0;
    EXPECT_EQ(call(SWITCH_VIC_GATE_RELEASE).error, 0);
}

TEST_F(SwitchVICGateTest, SurfacesVICCannotCopyAsTheyAreAreRefusedWithoutAJob)
{
    EXPECT_EQ(call(SWITCH_VIC_GATE_ARM, kTest, kSource, kSmall).error, 95) << "another size";
    EXPECT_EQ(call(SWITCH_VIC_GATE_ARM, kTest, kOdd, kDestination).error, 95) << "rows VIC cannot address";
    EXPECT_EQ(call(SWITCH_VIC_GATE_ARM, kTest, kSource, 0x99).error, 2) << "no such surface";
    transformResult = 0;
    EXPECT_EQ(call(SWITCH_VIC_GATE_ARM).error, 95) << "declined by the transform";
    transformResult = -12;
    EXPECT_EQ(call(SWITCH_VIC_GATE_ARM).error, 12);
    EXPECT_TRUE(submissions.empty());
    EXPECT_EQ(held, 0);
    EXPECT_EQ(call(SWITCH_VIC_GATE_QUERY).state, uint32_t(SWITCH_VIC_GATE_IDLE));
}

TEST_F(SwitchVICGateTest, MalformedRequestsAreBadArguments)
{
    IOReturn result = 0;
    call(9, kTest, kSource, kDestination, &result);
    EXPECT_EQ(result, kIOReturnBadArgument);
    const SwitchVICGateRequest request = {SWITCH_VIC_GATE_MAGIC, SWITCH_VIC_GATE_QUERY, 0, 0};
    SwitchVICGateReply reply = {};
    IOExternalMethodArguments a;
    a.structureInput = &request;
    a.structureInputSize = sizeof(request);
    a.structureOutput = &reply;
    a.structureOutputSize = sizeof(reply) - 4;
    EXPECT_EQ(switch_vic_gate_call(kTest, &a), kIOReturnBadArgument);
}
}  // namespace
