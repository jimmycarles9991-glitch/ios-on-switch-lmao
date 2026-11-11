// Host test of drivers/NativeMobileDisplay/SwitchSBIM.h: the dictionaries IORegistryEntrySetCFProperty hands
// setProperties for libEDR's enableSBIM (one key, a boolean), read as NativeLegacyDisplay reads its OSDictionary.
#include <CoreFoundation/CoreFoundation.h>
#include <gtest/gtest.h>

#include "../SwitchSBIM.h"

namespace {

// IORegistryEntrySetCFProperty(entry, key, value): setProperties gets {key: value}
CFDictionaryRef property(const char *key, CFTypeRef value)
{
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFDictionaryRef d = CFDictionaryCreate(kCFAllocatorDefault, reinterpret_cast<const void **>(&k), &value, 1,
                                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(k);
    return d;
}

bool answer(CFDictionaryRef d, IOReturn &status)
{
    CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, switch_sbim::kEnableKey, kCFStringEncodingUTF8);
    CFTypeRef v = CFDictionaryGetValue(d, key);
    CFRelease(key);
    const int value = v && CFGetTypeID(v) == CFBooleanGetTypeID() ? int(CFBooleanGetValue(CFBooleanRef(v))) : -1;
    return switch_sbim::answer(unsigned(CFDictionaryGetCount(d)), value, status);
}

}  // namespace

TEST(SwitchSBIM, EnablingIsUnsupportedSoLibEDRStopsAsking)
{
    CFDictionaryRef d = property("enableSBIM", kCFBooleanTrue);
    IOReturn status = kIOReturnSuccess;
    EXPECT_TRUE(answer(d, status));
    EXPECT_EQ(status, kIOReturnUnsupported);
    CFRelease(d);
}

TEST(SwitchSBIM, DisablingSucceeds)
{
    CFDictionaryRef d = property("enableSBIM", kCFBooleanFalse);
    IOReturn status = kIOReturnError;
    EXPECT_TRUE(answer(d, status));
    EXPECT_EQ(status, kIOReturnSuccess);
    CFRelease(d);
}

TEST(SwitchSBIM, OtherDictionariesAreTheFamilys)
{
    IOReturn status = kIOReturnError;
    CFDictionaryRef other = property("IOMFBSkipSpinner", kCFBooleanTrue);
    EXPECT_FALSE(answer(other, status));
    CFRelease(other);
    CFNumberRef one = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &status);
    CFDictionaryRef number = property("enableSBIM", one);
    EXPECT_FALSE(answer(number, status));
    CFRelease(number);
    CFRelease(one);
    EXPECT_EQ(status, kIOReturnError);
}
