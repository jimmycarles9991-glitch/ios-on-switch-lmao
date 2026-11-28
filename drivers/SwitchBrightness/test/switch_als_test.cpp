// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of drivers/SwitchBrightness/SwitchALS.cpp: starts off (sampleUS == 0) until ReportInterval > 0, powers
// down BH1730 (RegControl = 0) on ReportInterval == 0, suppresses unchanged IOHIDEvent allocations/dispatches, honors
// first-sample dispatch after wake/re-enable, retries on kIOReturnNotReady, and tightens timer leeway <= 10 ms.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../SwitchALS.cpp"

namespace {
struct I2CWrite {
    uint8_t cmd;
    bool hasVal;
    uint8_t val;
};

struct DispatchedEvent {
    uint32_t level;
    uint32_t visible;
    uint32_t ir;
};

std::vector<I2CWrite> gWrites;
std::vector<DispatchedEvent> gDispatched;
uint16_t gData0 = 372;
uint16_t gData1 = 132;
bool gDisplayOn = true;
IOReturn gDispatchStatus = kIOReturnSuccess;
struct FakeHidDevice : IOService {
    SwitchHIDKind kind = kSwitchHIDTouch;
    IOWorkLoop *sharedLoop = nullptr;

    void free() override
    {
        OSSafeReleaseNULL(sharedLoop);
        IOService::free();
    }
};

SwitchHIDFeatureHandler gFeatureHandler = nullptr;
void *gFeatureCtx = nullptr;
SwitchHIDCopyEventHandler gCopyHandler = nullptr;
void *gCopyCtx = nullptr;
FakeHidDevice *gHidDevice = nullptr;
unsigned gI2cInitCalls = 0;
unsigned gI2cReadCalls = 0;
int gEventDataSentinel = 1;

void fakeSetTimeStamp(OSObject *obj, uint64_t ts)
{
    reinterpret_cast<FakeHIDEvent *>(obj)->timestamp = ts;
}

void fakeSetType(OSObject *obj, uint32_t type)
{
    auto *e = reinterpret_cast<FakeHIDEvent *>(obj);
    e->type = type;
    e->data = &gEventDataSentinel;
}

void fakeSetIntegerValue(OSObject *obj, uint32_t field, int32_t value, IOOptionBits)
{
    auto *e = reinterpret_cast<FakeHIDEvent *>(obj);
    if (field == 0xc0000)
        e->level = value;
    else if (field == 0xc0001)
        e->ch0 = value;
    else if (field == 0xc0002)
        e->ch1 = value;
}

// Slot 1 in Itanium ABI is the deleting/complete dtor or release? SwitchALS calls event->release() which is non-virtual
// on stub OSObject — wait: SwitchALS allocates FakeHIDEvent as OSObject* and calls event->release() (non-virtual on
// stub OSObject), so FakeHIDEvent must have OSObject-compatible layout or inherit OSObject!
}  // namespace

// Let's make sure FakeHIDEvent uses a dedicated OSObject subclass with a custom vtable at offset 0 ONLY for slots
// 17, 19, 24 while keeping OSObject::release() safe. Let's inspect stub OSObject: OSObject::release() calls virtual
// free() and delete this, so if vtable is replaced, we shouldn't call OSObject::release() through the fake vtable —
// wait: in switch_als_stubs.h OSObject has `int refs` right after vptr (`offset 8`), so `FakeHIDEvent::data` at +0x10
// sits right after `vptr (8 bytes)` + `refs (4 bytes)` + `pad (4 bytes)`! Let's populate vtable slots 0/1/2 for dtor +
// free so `event->release()` cleanly frees `FakeHIDEvent`!
namespace {
void fakeNoop(OSObject *)
{}

void fakeDelete(OSObject *obj)
{
    delete reinterpret_cast<FakeHIDEvent *>(obj);
}

void initEventVtable()
{
    std::memset(OSMetaClass::eventVtable, 0, sizeof(OSMetaClass::eventVtable));
    OSMetaClass::eventVtable[0] = reinterpret_cast<uintptr_t>(&fakeNoop);
    OSMetaClass::eventVtable[1] = reinterpret_cast<uintptr_t>(&fakeDelete);
    OSMetaClass::eventVtable[2] = reinterpret_cast<uintptr_t>(&fakeNoop);  // virtual free()
    OSMetaClass::eventVtable[17] = reinterpret_cast<uintptr_t>(&fakeSetTimeStamp);
    OSMetaClass::eventVtable[19] = reinterpret_cast<uintptr_t>(&fakeSetType);
    OSMetaClass::eventVtable[24] = reinterpret_cast<uintptr_t>(&fakeSetIntegerValue);
}

void setReportInterval(uint32_t us)
{
    ASSERT_NE(gFeatureHandler, nullptr);
    const uint8_t report[5] = {
        1,
        static_cast<uint8_t>(us & 0xff),
        static_cast<uint8_t>((us >> 8) & 0xff),
        static_cast<uint8_t>((us >> 16) & 0xff),
        static_cast<uint8_t>((us >> 24) & 0xff),
    };
    gFeatureHandler(gFeatureCtx, report, sizeof(report));
}
}  // namespace

OSObject *OSMetaClass::allocClassWithName(const char *)
{
    ++allocs;
    auto *e = new FakeHIDEvent{};
    e->vtable = eventVtable;
    // Set OSObject's refs field (at +0x08, same as e->pad's low 32 bits) to 1 so OSObject::release() deletes it.
    int one = 1;
    std::memcpy(&e->pad, &one, sizeof(one));
    return reinterpret_cast<OSObject *>(e);
}

extern "C" {
int tegra_i2c_init(unsigned)
{
    ++gI2cInitCalls;
    return TEGRA_I2C_OK;
}

int tegra_i2c_read(unsigned, uint8_t, uint8_t reg, void *buf, size_t len)
{
    ++gI2cReadCalls;
    auto *out = static_cast<uint8_t *>(buf);
    if (reg == (0x80 | 0x12) && len == 1) {
        out[0] = 0x71;  // BH1730 PART_ID (upper nibble 7)
        return TEGRA_I2C_OK;
    }
    if (reg == (0x80 | 0x14) && len == 4) {
        out[0] = static_cast<uint8_t>(gData0 & 0xff);
        out[1] = static_cast<uint8_t>(gData0 >> 8);
        out[2] = static_cast<uint8_t>(gData1 & 0xff);
        out[3] = static_cast<uint8_t>(gData1 >> 8);
        return TEGRA_I2C_OK;
    }
    std::memset(out, 0, len);
    return TEGRA_I2C_OK;
}

int tegra_i2c_write(unsigned, uint8_t, uint8_t reg, const void *buf, size_t len)
{
    const uint8_t val = (buf && len) ? *static_cast<const uint8_t *>(buf) : 0;
    gWrites.push_back({reg, len != 0, val});
    return TEGRA_I2C_OK;
}

int tegra_i2c_transfer(unsigned, uint8_t, const void *, size_t, void *, size_t)
{
    return TEGRA_I2C_ENACK;
}

void tegra_i2c_describe(unsigned, char *buf, size_t size)
{
    std::snprintf(buf, size, "i2c stub");
}

int tegra_gpio_read(unsigned)
{
    return 1;
}

int tegra_gpio_sample(unsigned, unsigned *cfg)
{
    if (cfg)
        *cfg = 0;
    return 1;
}

uint32_t switch_pmc_read(uint32_t)
{
    return 0;
}

bool switch_display_is_on(void)
{
    return gDisplayOn;
}

void switch_refusal(const char *, const char *, const char *, ...)
{}
}  // extern "C"

IOService *switch_hid_create(SwitchHIDKind kind, IOWorkLoop *sharedLoop)
{
    auto *dev = new FakeHidDevice;
    dev->kind = kind;
    if (sharedLoop) {
        sharedLoop->retain();
        dev->sharedLoop = sharedLoop;
    }
    gHidDevice = dev;
    return dev;
}

IOService *switch_hid_create(SwitchHIDKind kind)
{
    return switch_hid_create(kind, nullptr);
}

void switch_hid_set_feature_handler(IOService *, SwitchHIDFeatureHandler handler, void *context)
{
    gFeatureHandler = handler;
    gFeatureCtx = context;
}

void switch_hid_set_light_copy_handler(IOService *, SwitchHIDCopyEventHandler handler, void *context)
{
    gCopyHandler = handler;
    gCopyCtx = context;
}

IOReturn switch_hid_dispatch_event(IOService *, OSObject *event)
{
    const auto *e = reinterpret_cast<const FakeHIDEvent *>(event);
    if (gDispatchStatus == kIOReturnSuccess) {
        gDispatched.push_back(
            {static_cast<uint32_t>(e->level), static_cast<uint32_t>(e->ch0), static_cast<uint32_t>(e->ch1)});
    }
    return gDispatchStatus;
}

namespace {
void resetALS()
{
    if (gALS) {
        gALS->detach(IOService::getResourceService());
        gALS = nullptr;
    }
    if (gHidDevice) {
        gHidDevice->release();
        gHidDevice = nullptr;
    }
    gSupply = false;
    gFeatureHandler = nullptr;
    gFeatureCtx = nullptr;
    gCopyHandler = nullptr;
    gCopyCtx = nullptr;
    gWrites.clear();
    gDispatched.clear();
    OSMetaClass::allocs = 0;
    IOTimerEventSource::all.clear();
    IOWorkLoop::last = nullptr;
    IOWorkLoop::created = 0;
    IOWorkLoop::runActionCalls = 0;
    gI2cInitCalls = 0;
    gI2cReadCalls = 0;
    gNowNS = 1000000000ULL;
    gDisplayOn = true;
    gDispatchStatus = kIOReturnSuccess;
}
}  // namespace

TEST(SwitchALS, StartsOffHonorsReportIntervalZeroDeadbandAndWake)
{
    resetALS();
    initEventVtable();
    gData0 = 372;
    gData1 = 132;  // 47 lux at 64x

    ASSERT_EQ(switch_als_start(), 0);
    ASSERT_NE(gCopyHandler, nullptr);
    ASSERT_GE(IOTimerEventSource::all.size(), 2u);
    IOTimerEventSource *clock = IOTimerEventSource::all[0];

    // Supply arrives while ReportInterval is still 0: sensor stays OFF, zero I2C traffic, clock disarmed.
    switch_als_supply(true);
    EXPECT_FALSE(clock->armed);
    EXPECT_TRUE(gWrites.empty());
    EXPECT_EQ(gI2cInitCalls, 0u);
    EXPECT_EQ(OSMetaClass::allocs, 0u);

    // Client requests ReportInterval = 250000 us: configures BH1730 and arms kFirstSampleUS.
    setReportInterval(250000);
    EXPECT_TRUE(clock->armed);
    EXPECT_EQ(clock->lastIntervalUS, switch_als::kFirstSampleUS);
    ASSERT_FALSE(gWrites.empty());
    EXPECT_EQ(gWrites.back().cmd, static_cast<uint8_t>(0x80 | 0x00));  // RegControl
    EXPECT_EQ(gWrites.back().val, static_cast<uint8_t>(0x03));         // kPowerOn | kAdcEnable

    // First tick with kIOReturnNotReady does NOT latch deadband; retries on next tick.
    gDispatchStatus = -1;
    clock->fire();
    EXPECT_EQ(clock->lastIntervalUS, 250000u);
    EXPECT_LE(clock->lastLeewayUS, 10000u);
    EXPECT_TRUE(gDispatched.empty());
    EXPECT_EQ(OSMetaClass::allocs, 1u);

    // Next tick succeeds: first sample dispatched.
    gDispatchStatus = kIOReturnSuccess;
    clock->fire();
    ASSERT_EQ(gDispatched.size(), 1u);
    EXPECT_EQ(gDispatched[0].level, 47u);
    EXPECT_EQ(OSMetaClass::allocs, 2u);

    // Identical and tiny-jitter readings within deadband allocate ZERO IOHIDEvents and dispatch ZERO events.
    clock->fire();
    gData0 = 376;  // still ~47-48 lux (within deadband of 47: diff <= 47/32 == 1)
    clock->fire();
    EXPECT_EQ(gDispatched.size(), 1u);
    EXPECT_EQ(OSMetaClass::allocs, 2u);

    // Meaningful light change dispatches immediately.
    gData0 = 600;
    gData1 = 210;
    clock->fire();
    ASSERT_EQ(gDispatched.size(), 2u);
    EXPECT_GT(gDispatched[1].level, 60u);
    EXPECT_EQ(OSMetaClass::allocs, 3u);

    // ReportInterval == 0 stops timer and powers down BH1730 (RegControl = 0).
    gWrites.clear();
    setReportInterval(0);
    EXPECT_FALSE(clock->armed);
    ASSERT_EQ(gWrites.size(), 1u);
    EXPECT_EQ(gWrites[0].cmd, static_cast<uint8_t>(0x80 | 0x00));
    EXPECT_EQ(gWrites[0].val, 0u);

    // Re-enabling with the exact same reading still dispatches the first sample.
    setReportInterval(250000);
    EXPECT_TRUE(clock->armed);
    clock->fire();
    ASSERT_EQ(gDispatched.size(), 3u);
    EXPECT_EQ(gDispatched[2].level, gDispatched[1].level);

    // Display off -> on wake powers down, then restarts and always dispatches the first sample.
    gWrites.clear();
    switch_als_set_active(false);
    EXPECT_FALSE(clock->armed);
    ASSERT_EQ(gWrites.size(), 1u);
    EXPECT_EQ(gWrites[0].val, 0u);

    switch_als_set_active(true);
    EXPECT_TRUE(clock->armed);
    clock->fire();
    ASSERT_EQ(gDispatched.size(), 4u);
}

TEST(SwitchALS, CopyEventOnDemandCacheAndPowerGating)
{
    resetALS();
    initEventVtable();
    gData0 = 372;
    gData1 = 132;  // 47 lux at 64x

    ASSERT_EQ(switch_als_start(), 0);
    ASSERT_NE(gCopyHandler, nullptr);
    ASSERT_GE(IOTimerEventSource::all.size(), 2u);
    IOTimerEventSource *clock = IOTimerEventSource::all[0];

    // Before LDO6 supply arrives: copyEvent returns nullptr and touches neither clock nor I2C.
    EXPECT_EQ(gCopyHandler(gCopyCtx), nullptr);
    EXPECT_FALSE(clock->armed);
    EXPECT_TRUE(gWrites.empty());

    // Supply arrives while ReportInterval == 0: first copyEvent kicks async one-shot and returns nullptr immediately.
    switch_als_supply(true);
    EXPECT_EQ(gCopyHandler(gCopyCtx), nullptr);
    EXPECT_TRUE(clock->armed);
    EXPECT_EQ(clock->lastIntervalUS, switch_als::kFirstSampleUS);
    ASSERT_FALSE(gWrites.empty());
    EXPECT_EQ(gWrites.back().cmd, static_cast<uint8_t>(0x80 | 0x00));
    EXPECT_EQ(gWrites.back().val, static_cast<uint8_t>(0x03));

    // One-shot tick reads BH1730, updates cache without unsolicited streaming dispatch, and powers down.
    gWrites.clear();
    gNowNS += 150000000ULL;
    const uint64_t sampleStamp = gNowNS;
    clock->fire();
    EXPECT_FALSE(clock->armed);
    EXPECT_TRUE(gDispatched.empty());
    ASSERT_EQ(gWrites.size(), 1u);
    EXPECT_EQ(gWrites[0].cmd, static_cast<uint8_t>(0x80 | 0x00));
    EXPECT_EQ(gWrites[0].val, 0u);

    // Subsequent copyEvent within freshness window returns the cached reading with zero I2C traffic.
    gWrites.clear();
    gNowNS += 1000000000ULL;
    OSObject *ev = gCopyHandler(gCopyCtx);
    ASSERT_NE(ev, nullptr);
    auto *fe = reinterpret_cast<FakeHIDEvent *>(ev);
    EXPECT_EQ(fe->type, 12u);
    EXPECT_EQ(fe->level, 47);
    EXPECT_EQ(fe->ch0, 372);
    EXPECT_EQ(fe->ch1, 132);
    EXPECT_EQ(fe->timestamp, sampleStamp);
    ev->release();
    EXPECT_FALSE(clock->armed);
    EXPECT_TRUE(gWrites.empty());

    // Stale copyEvent (>= 4 s later) returns cached event immediately and schedules an async one-shot refresh.
    gData0 = 600;
    gData1 = 210;
    gNowNS += 4000000000ULL;
    ev = gCopyHandler(gCopyCtx);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(reinterpret_cast<FakeHIDEvent *>(ev)->level, 47);
    ev->release();
    EXPECT_TRUE(clock->armed);

    gNowNS += 150000000ULL;
    const uint64_t refreshedStamp = gNowNS;
    clock->fire();
    EXPECT_FALSE(clock->armed);
    ev = gCopyHandler(gCopyCtx);
    ASSERT_NE(ev, nullptr);
    fe = reinterpret_cast<FakeHIDEvent *>(ev);
    EXPECT_EQ(fe->level, static_cast<int32_t>(switch_als::lux(600, 210, 2)));
    EXPECT_EQ(fe->ch0, 600);
    EXPECT_EQ(fe->ch1, 210);
    EXPECT_EQ(fe->timestamp, refreshedStamp);
    ev->release();

    // Screen off and suspend gate copyEvent: returns nullptr with zero clock/I2C activity.
    gWrites.clear();
    gNowNS += 5000000000ULL;
    switch_als_set_active(false);
    EXPECT_EQ(gCopyHandler(gCopyCtx), nullptr);
    EXPECT_FALSE(clock->armed);
    EXPECT_TRUE(gWrites.empty());

    switch_als_set_active(true);
    switch_als_suspend();
    EXPECT_EQ(gCopyHandler(gCopyCtx), nullptr);
    EXPECT_FALSE(clock->armed);
    EXPECT_TRUE(gWrites.empty());

    // Resume and verify saturated one-shot steps gain down, skips one conversion, records reading, and powers down.
    switch_als_resume();
    gData0 = 65535;
    gData1 = 10000;
    ev = gCopyHandler(gCopyCtx);
    ASSERT_NE(ev, nullptr);
    ev->release();
    EXPECT_TRUE(clock->armed);

    // Tick 1: saturated at 64x -> steps gain to 2x, sets skip = 1, stays armed.
    gNowNS += 150000000ULL;
    clock->fire();
    EXPECT_TRUE(clock->armed);
    // Tick 2: skipped conversion in flight at old gain, stays armed.
    gData0 = 2000;
    gData1 = 700;
    gNowNS += 150000000ULL;
    clock->fire();
    EXPECT_TRUE(clock->armed);
    // Tick 3: valid 2x reading recorded and sensor powered down.
    gNowNS += 150000000ULL;
    const uint64_t gainStamp = gNowNS;
    clock->fire();
    EXPECT_FALSE(clock->armed);
    ev = gCopyHandler(gCopyCtx);
    ASSERT_NE(ev, nullptr);
    fe = reinterpret_cast<FakeHIDEvent *>(ev);
    EXPECT_EQ(fe->level, static_cast<int32_t>(switch_als::lux(2000, 700, 1)));
    EXPECT_EQ(fe->ch0, 2000);
    EXPECT_EQ(fe->ch1, 700);
    EXPECT_EQ(fe->timestamp, gainStamp);
    ev->release();
}

TEST(SwitchALS, SharesWorkLoopWithHidDevice)
{
    resetALS();
    initEventVtable();

    ASSERT_EQ(switch_als_start(), 0);
    EXPECT_EQ(IOWorkLoop::created, 1u);
    ASSERT_NE(IOWorkLoop::last, nullptr);
    ASSERT_NE(gHidDevice, nullptr);
    EXPECT_EQ(gHidDevice->kind, kSwitchHIDLight);
    EXPECT_EQ(gHidDevice->sharedLoop, IOWorkLoop::last);

    switch_als_supply(true);
    const unsigned runActionsBefore = IOWorkLoop::runActionCalls;
    setReportInterval(250000);
    EXPECT_EQ(IOWorkLoop::runActionCalls, runActionsBefore);
    setReportInterval(0);
    EXPECT_EQ(IOWorkLoop::runActionCalls, runActionsBefore);
    EXPECT_EQ(gCopyHandler(gCopyCtx), nullptr);
    EXPECT_EQ(IOWorkLoop::runActionCalls, runActionsBefore);

    // SwitchALS::free() drops its loop reference while the HID device keeps its retained reference.
    resetALS();
}
