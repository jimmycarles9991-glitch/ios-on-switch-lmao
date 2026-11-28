// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of drivers/SwitchTouch/SwitchHIDFastPath.cpp: vtable slot 206 ("QueueSize" == 4096 + delegation),
// NativeSandboxGrantProcess, IOCFPlugInTypes, openForClient aonBuffer refusal, dispatch->keep->copyEventForClient,
// switch_hid_forget_latest, and re-match cleanup.
#include <gtest/gtest.h>

#include <cstring>

#include "../SwitchHIDFastPath.cpp"

namespace {
constexpr unsigned kFakeSlots = 256;
uintptr_t gOrigVtable[kFakeSlots];

// Map fake objects through a real IOService subclass whose vtable slot layout we control directly via a fake vtable
// array: SwitchHIDFastPath only calls getProvider/getProperty/setProperty (non-virtual on stub IOService) and reads
// *reinterpret_cast<uintptr_t **>(driver) for slots 198, 199, 200, 205, 206, 208, 209.
struct TestEventDriver : IOService {
    uintptr_t *savedVtable = nullptr;

    void installFakeVtable(uintptr_t *vt)
    {
        savedVtable = *reinterpret_cast<uintptr_t **>(this);
        *reinterpret_cast<uintptr_t **>(this) = vt;
    }

    void restoreVtable()
    {
        if (savedVtable) {
            *reinterpret_cast<uintptr_t **>(this) = savedVtable;
            savedVtable = nullptr;
        }
    }

    ~TestEventDriver() override
    {
        restoreVtable();
    }
};

uint32_t gUsagePage = 0xff0c;
uint32_t gUsage = 5;
unsigned gOrigDispatches = 0;
unsigned gOrigCopyProps = 0;
OSObject *gOrigPropResult = nullptr;
bool gCopiedDuringOrigDispatch = false;

uint32_t fakePrimaryUsagePage(void *)
{
    return gUsagePage;
}

uint32_t fakePrimaryUsage(void *)
{
    return gUsage;
}

void fakeOrigDispatch(IOService *driver, OSObject *, IOOptionBits)
{
    ++gOrigDispatches;
    // The event must not be visible to copyEventForClient before originalDispatch finishes.
    OSObject *early = call<OSObject *>(driver, kCopyEventForClient, static_cast<OSObject *>(nullptr), IOOptionBits{0},
                                       static_cast<void *>(nullptr));
    gCopiedDuringOrigDispatch = early != nullptr;
    OSSafeReleaseNULL(early);
}

OSObject *fakeOrigCopyProperty(const IOService *, const char *, void *)
{
    ++gOrigCopyProps;
    if (gOrigPropResult)
        gOrigPropResult->retain();
    return gOrigPropResult;
}

void initOrigVtable()
{
    std::memset(gOrigVtable, 0, sizeof(gOrigVtable));
    gOrigVtable[kDispatch] = reinterpret_cast<uintptr_t>(&fakeOrigDispatch);
    gOrigVtable[kPrimaryUsagePage] = reinterpret_cast<uintptr_t>(&fakePrimaryUsagePage);
    gOrigVtable[kPrimaryUsage] = reinterpret_cast<uintptr_t>(&fakePrimaryUsage);
    gOrigVtable[kCopyPropertyForClient] = reinterpret_cast<uintptr_t>(&fakeOrigCopyProperty);
}
}  // namespace

TEST(SwitchHIDFastPath, InstallsSlotsQueueSizeSandboxGrantAndCopiesEvents)
{
    initOrigVtable();
    auto *motionDev = new IOService;
    auto *iface = new IOService;
    iface->setProvider(motionDev);
    auto *driver = new TestEventDriver;
    driver->setProvider(iface);
    driver->installFakeVtable(gOrigVtable);

    switch_hid_fastpath_watch(motionDev);
    ASSERT_NE(IOService::gHandler, nullptr);
    EXPECT_TRUE(IOService::gHandler(IOService::gTarget, IOService::gRefCon, driver, nullptr));

    // NativeSandboxGrantProcess and IOCFPlugInTypes are both published on the event driver.
    const auto *grant = OSDynamicCast(OSString, driver->getProperty("NativeSandboxGrantProcess"));
    ASSERT_NE(grant, nullptr);
    EXPECT_STREQ(grant->getCStringNoCopy(), "backboardd,SpringBoard,locationd");

    const auto *plugins = OSDynamicCast(OSDictionary, driver->getProperty("IOCFPlugInTypes"));
    ASSERT_NE(plugins, nullptr);
    const auto *plug = OSDynamicCast(OSString, plugins->getObject(kPlugInType));
    ASSERT_NE(plug, nullptr);
    EXPECT_STREQ(plug->getCStringNoCopy(), kPlugIn);

    // Slot 206 answers QueueSize == 4096 and delegates every other key (and nullptr) to original slot 206.
    OSObject *qObj = call<OSObject *>(driver, kCopyPropertyForClient, "QueueSize", static_cast<void *>(nullptr));
    const auto *qNum = OSDynamicCast(OSNumber, qObj);
    ASSERT_NE(qNum, nullptr);
    EXPECT_EQ(qNum->unsigned32BitValue(), 4096u);
    OSSafeReleaseNULL(qObj);

    gOrigCopyProps = 0;
    gOrigPropResult = OSString::withCString("delegated");
    OSObject *other = call<OSObject *>(driver, kCopyPropertyForClient, "OtherKey", static_cast<void *>(nullptr));
    EXPECT_EQ(gOrigCopyProps, 1u);
    ASSERT_NE(OSDynamicCast(OSString, other), nullptr);
    EXPECT_STREQ(OSDynamicCast(OSString, other)->getCStringNoCopy(), "delegated");
    OSSafeReleaseNULL(other);
    OSObject *nullKey = call<OSObject *>(driver, kCopyPropertyForClient, static_cast<const char *>(nullptr),
                                         static_cast<void *>(nullptr));
    EXPECT_EQ(gOrigCopyProps, 2u);
    OSSafeReleaseNULL(nullKey);
    OSSafeReleaseNULL(gOrigPropResult);

    // Slot 208 admits normal opens and rejects aonBuffer = true.
    void *ctx = reinterpret_cast<void *>(0x1234);
    EXPECT_TRUE(call<bool>(driver, kOpenForClient, static_cast<IOService *>(nullptr), IOOptionBits{0},
                           static_cast<OSDictionary *>(nullptr), &ctx));
    EXPECT_EQ(ctx, nullptr);

    OSDictionary *aonProps = OSDictionary::withCapacity(1);
    auto *yes = new OSBoolean(true);
    aonProps->setObject("aonBuffer", yes);
    EXPECT_FALSE(
        call<bool>(driver, kOpenForClient, static_cast<IOService *>(nullptr), IOOptionBits{0}, aonProps, &ctx));
    OSSafeReleaseNULL(yes);
    OSSafeReleaseNULL(aonProps);

    // Before any dispatch, copyEventForClient returns nullptr and does not increment copies.
    const uint64_t copiesBefore = switch_hid_fastpath_copies();
    EXPECT_EQ(call<OSObject *>(driver, kCopyEventForClient, static_cast<OSObject *>(nullptr), IOOptionBits{0},
                               static_cast<void *>(nullptr)),
              nullptr);
    EXPECT_EQ(switch_hid_fastpath_copies(), copiesBefore);

    // Dispatch calls originalDispatch first (event not yet exposed), then keeps event for copyEventForClient.
    auto *evt = new OSObject;
    gOrigDispatches = 0;
    gCopiedDuringOrigDispatch = true;
    call<void>(driver, kDispatch, evt, IOOptionBits{0});
    EXPECT_EQ(gOrigDispatches, 1u);
    EXPECT_FALSE(gCopiedDuringOrigDispatch);

    OSObject *copied = call<OSObject *>(driver, kCopyEventForClient, static_cast<OSObject *>(nullptr), IOOptionBits{0},
                                        static_cast<void *>(nullptr));
    EXPECT_EQ(copied, evt);
    EXPECT_EQ(switch_hid_fastpath_copies(), copiesBefore + 1);
    OSSafeReleaseNULL(copied);

    // Forget clears latest; subsequent copy returns nullptr and leaves copies unchanged.
    switch_hid_forget_latest(motionDev);
    EXPECT_EQ(call<OSObject *>(driver, kCopyEventForClient, static_cast<OSObject *>(nullptr), IOOptionBits{0},
                               static_cast<void *>(nullptr)),
              nullptr);
    EXPECT_EQ(switch_hid_fastpath_copies(), copiesBefore + 1);
    EXPECT_EQ(evt->refCount(), 1);
    OSSafeReleaseNULL(evt);

    // Re-match on a fresh driver clears any kept event and hooks the new driver.
    auto *evt2 = new OSObject;
    call<void>(driver, kDispatch, evt2, IOOptionBits{0});
    auto *driver2 = new TestEventDriver;
    driver2->setProvider(iface);
    driver2->installFakeVtable(gOrigVtable);
    EXPECT_TRUE(IOService::gHandler(IOService::gTarget, IOService::gRefCon, driver2, nullptr));
    EXPECT_EQ(call<OSObject *>(driver2, kCopyEventForClient, static_cast<OSObject *>(nullptr), IOOptionBits{0},
                               static_cast<void *>(nullptr)),
              nullptr);
    EXPECT_EQ(evt2->refCount(), 1);
    OSSafeReleaseNULL(evt2);

    driver2->restoreVtable();
    driver->restoreVtable();
    driver2->release();
    driver->release();
    iface->release();
    motionDev->release();
}
