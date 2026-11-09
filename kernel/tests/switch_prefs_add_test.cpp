// kernel/switch/boot/prefs.c's switch_prefs_add_integer against CoreFoundation's own binary plist writer and reader
// (the code cfprefsd runs): CoreBrightness's plist gains CBColorAdaptationEnabled = 0 only where it lacks the key,
// every other key and value as it was; and the guard that only iOS 27's kernel sets it, at every boot, before cfprefsd.
#include <gtest/gtest.h>

#include <CoreFoundation/CoreFoundation.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

extern "C" {
#include <switch/prefs.h>
}

namespace {
using Bytes = std::vector<uint8_t>;

const char kKey[] = "CBColorAdaptationEnabled";

struct Ref {
    CFTypeRef ref;

    explicit Ref(CFTypeRef r) : ref(r)
    {}

    ~Ref()
    {
        if (ref)
            CFRelease(ref);
    }

    Ref(const Ref &) = delete;
    Ref &operator=(const Ref &) = delete;
};

CFStringRef Str(const char *text)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, text, kCFStringEncodingUTF8);
}

void SetInt(CFMutableDictionaryRef dict, const char *key, long long value)
{
    Ref k(Str(key)), v(CFNumberCreate(kCFAllocatorDefault, kCFNumberLongLongType, &value));
    CFDictionarySetValue(dict, k.ref, v.ref);
}

CFMutableDictionaryRef NewDict()
{
    return CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
}

Bytes Binary(CFPropertyListRef plist)
{
    Ref data(CFPropertyListCreateData(kCFAllocatorDefault, plist, kCFPropertyListBinaryFormat_v1_0, 0, nullptr));
    const auto *bytes = CFDataGetBytePtr(static_cast<CFDataRef>(data.ref));
    return Bytes(bytes, bytes + CFDataGetLength(static_cast<CFDataRef>(data.ref)));
}

CFPropertyListRef Parse(const Bytes &bytes)
{
    Ref data(CFDataCreate(kCFAllocatorDefault, bytes.data(), static_cast<CFIndex>(bytes.size())));
    return CFPropertyListCreateWithData(kCFAllocatorDefault, static_cast<CFDataRef>(data.ref), kCFPropertyListImmutable,
                                        nullptr, nullptr);
}

switch_prefs_result Add(const Bytes &in, Bytes &out, long long value = 0)
{
    size_t length = switch_prefs_add_room(in.size());
    out.assign(length, 0xee);
    const switch_prefs_result result = switch_prefs_add_integer(in.data(), in.size(), kKey, value, out.data(), &length);
    out.resize(result == SWITCH_PREFS_ADDED ? length : 0);
    return result;
}

long long IntValue(CFDictionaryRef dict, const char *key)
{
    Ref k(Str(key));
    const void *value = CFDictionaryGetValue(dict, k.ref);
    long long n = -12345;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &n);
    return n;
}

// CoreBrightness's plist as a device's holds it: Night Shift's colour temperature target and an "internal-0"
// dictionary of auto-brightness's learned preference (dictionaries, an array, reals), plus `extra` keys to reach the
// wider encodings.
CFMutableDictionaryRef CoreBrightnessPlist(int extra)
{
    CFMutableDictionaryRef dict = NewDict();
    SetInt(dict, "CBBlueLightReductionCCTTargetRaw", 4100);
    Ref points(CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks));
    for (const double lux : {0.0, 120.5, 4000.0}) {
        Ref point(NewDict());
        Ref luxValue(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &lux)), luxName(Str("Lux"));
        CFDictionarySetValue(static_cast<CFMutableDictionaryRef>(const_cast<void *>(point.ref)), luxName.ref,
                             luxValue.ref);
        SetInt(static_cast<CFMutableDictionaryRef>(const_cast<void *>(point.ref)), "Nits",
               static_cast<long long>(lux / 10));
        CFArrayAppendValue(static_cast<CFMutableArrayRef>(const_cast<void *>(points.ref)), point.ref);
    }
    Ref preference(NewDict()), pointsName(Str("Points"));
    CFDictionarySetValue(static_cast<CFMutableDictionaryRef>(const_cast<void *>(preference.ref)), pointsName.ref,
                         points.ref);
    Ref internal(NewDict()), preferenceName(Str("ALSUserPreference")), internalName(Str("internal-0"));
    CFDictionarySetValue(static_cast<CFMutableDictionaryRef>(const_cast<void *>(internal.ref)), preferenceName.ref,
                         preference.ref);
    CFDictionarySetValue(dict, internalName.ref, internal.ref);
    for (int i = 0; i < extra; ++i) {
        char key[40];
        snprintf(key, sizeof(key), "CBExtraSetting%04d", i);
        SetInt(dict, key, 70000 + i);
    }
    return dict;
}

struct Trailer {
    unsigned offset_size, ref_size;
    uint64_t objects;
};

Trailer TrailerOf(const Bytes &plist)
{
    const uint8_t *t = plist.data() + plist.size() - 32;
    uint64_t objects = 0;
    for (int i = 8; i < 16; ++i)
        objects = objects << 8 | t[i];
    return {t[6], t[7], objects};
}

void ExpectAddedRoundTrip(CFDictionaryRef original, const Bytes &in, Bytes &out, int extra)
{
    ASSERT_EQ(Add(in, out), SWITCH_PREFS_ADDED) << extra;
    Ref parsed(Parse(out));
    ASSERT_NE(parsed.ref, nullptr) << extra << ": CoreFoundation does not read the result";
    const auto *dict = static_cast<CFDictionaryRef>(parsed.ref);
    const CFIndex count = CFDictionaryGetCount(original);
    EXPECT_EQ(CFDictionaryGetCount(dict), count + 1) << extra;
    EXPECT_EQ(IntValue(dict, kKey), 0) << extra;
    std::vector<const void *> keys(static_cast<size_t>(count)), values(static_cast<size_t>(count));
    CFDictionaryGetKeysAndValues(original, keys.data(), values.data());
    for (size_t i = 0; i < keys.size(); ++i) {
        const void *now = CFDictionaryGetValue(dict, keys[i]);
        EXPECT_TRUE(now && CFEqual(now, values[i])) << extra << ": entry " << i << " changed";
    }
    Bytes again;
    EXPECT_EQ(Add(out, again), SWITCH_PREFS_UNCHANGED) << extra << ": a second boot adds nothing";
}

// From a few keys to past one-byte references, through the 15-entry dictionary marker and offsets that widen.
TEST(SwitchPrefsAdd, BinaryGainsTheKeyAndKeepsTheRest)
{
    bool widened = false, refused = false, wide_references = false;
    for (int extra = 0; extra <= 300; ++extra) {
        Ref original(CoreBrightnessPlist(extra));
        const Bytes in = Binary(original.ref);
        const Trailer before = TrailerOf(in);
        Bytes out;
        if (before.ref_size == 1 && before.objects + 3 > 256) {
            EXPECT_EQ(Add(in, out), SWITCH_PREFS_UNREADABLE) << extra
                                                             << ": one-byte references cannot reach the new "
                                                                "objects";
            refused = true;
            continue;
        }
        ExpectAddedRoundTrip(static_cast<CFDictionaryRef>(original.ref), in, out, extra);
        if (HasFatalFailure())
            return;
        widened |= TrailerOf(out).offset_size > before.offset_size;
        wide_references |= before.ref_size > 1;
    }
    EXPECT_TRUE(widened);
    EXPECT_TRUE(refused);
    EXPECT_TRUE(wide_references);
}

TEST(SwitchPrefsAdd, TheUsersChoiceStays)
{
    for (long long choice : {0, 1}) {
        Ref dict(CoreBrightnessPlist(3));
        SetInt(static_cast<CFMutableDictionaryRef>(const_cast<void *>(dict.ref)), kKey, choice);
        Bytes out;
        EXPECT_EQ(Add(Binary(dict.ref), out), SWITCH_PREFS_UNCHANGED) << choice;
    }
}

TEST(SwitchPrefsAdd, AnEmptyDictionaryAndAnEmptyFile)
{
    Ref empty(NewDict());
    Bytes out;
    ASSERT_EQ(Add(Binary(empty.ref), out), SWITCH_PREFS_ADDED);
    Ref parsed(Parse(out));
    ASSERT_NE(parsed.ref, nullptr);
    EXPECT_EQ(CFDictionaryGetCount(static_cast<CFDictionaryRef>(parsed.ref)), 1);
    EXPECT_EQ(IntValue(static_cast<CFDictionaryRef>(parsed.ref), kKey), 0);

    const std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict/></plist>\n";
    ASSERT_EQ(Add(Bytes(xml.begin(), xml.end()), out), SWITCH_PREFS_ADDED);
    Ref seeded(Parse(out));
    ASSERT_NE(seeded.ref, nullptr) << std::string(out.begin(), out.end());
    EXPECT_EQ(IntValue(static_cast<CFDictionaryRef>(seeded.ref), kKey), 0);
    Bytes again;
    EXPECT_EQ(Add(out, again), SWITCH_PREFS_UNCHANGED);
}

TEST(SwitchPrefsAdd, XmlGainsTheKeyAtTheTop)
{
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST "
        "1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n"
        "<dict>\n\t<key>CBBlueLightReductionCCTTargetRaw</key>\n\t<integer>4100</integer>\n</dict>\n"
        "</plist>\n";
    Bytes out;
    ASSERT_EQ(Add(Bytes(xml.begin(), xml.end()), out, 1), SWITCH_PREFS_ADDED);
    Ref parsed(Parse(out));
    ASSERT_NE(parsed.ref, nullptr) << std::string(out.begin(), out.end());
    EXPECT_EQ(IntValue(static_cast<CFDictionaryRef>(parsed.ref), kKey), 1);
    EXPECT_EQ(IntValue(static_cast<CFDictionaryRef>(parsed.ref), "CBBlueLightReductionCCTTargetRaw"), 4100);
}

TEST(SwitchPrefsAdd, WhatItCannotEditItLeaves)
{
    Bytes out;
    const std::string text = "hello";
    EXPECT_EQ(Add(Bytes(text.begin(), text.end()), out), SWITCH_PREFS_UNREADABLE);
    const void *items[] = {kCFBooleanTrue};
    Ref array(CFArrayCreate(kCFAllocatorDefault, items, 1, &kCFTypeArrayCallBacks));
    EXPECT_EQ(Add(Binary(array.ref), out), SWITCH_PREFS_UNREADABLE) << "the top object is not a dictionary";
    Ref dict(CoreBrightnessPlist(5));
    Bytes in = Binary(dict.ref);
    size_t length = 16;
    Bytes small(length);
    EXPECT_EQ(switch_prefs_add_integer(in.data(), in.size(), kKey, 0, small.data(), &length), SWITCH_PREFS_UNREADABLE)
        << "a buffer too small is refused, not overrun";
}

// Only 27's kernel adds the key, every boot from the first-boot setup, which runs before cfprefsd.
TEST(SwitchPrefsAddGuard, TrueToneDefaultIsTwentySevensAlone)
{
    using switch_ios::test::Body;
    using switch_ios::test::Count;
    using switch_ios::test::SourceText;
    const std::string seventeen = SourceText("../xnu-10063.141.1/bsd/kern/kern_exec.c");
    const std::string twentySeven = SourceText("../xnu-12377.121.6/bsd/kern/kern_exec.c");
    EXPECT_EQ(Count(seventeen, kKey), 0u);
    const std::string body =
        Body(twentySeven, "native_switch_true_tone_default(proc_t p, user_addr_t scratch, mount_t data_mount)");
    ASSERT_FALSE(body.empty());
    EXPECT_EQ(Count(body, "\"/private/var/mobile/Library/Preferences/com.apple.CoreBrightness.plist\""), 1u);
    EXPECT_EQ(Count(twentySeven, "ByHost/com.apple.CoreBrightness"), 0u)
        << "iOS's CoreFoundation reads the current user's per-host values from the plain file";
    EXPECT_EQ(Count(body, "switch_prefs_add_integer(source, source_length, key, 0, out, &edited)"), 1u);
    EXPECT_EQ(Count(body, "501, 501, 0600)"), 1u);
    EXPECT_EQ(Count(Body(twentySeven, "native_switch_setup_state(proc_t p, user_addr_t scratch, mount_t data_mount)"),
                    "native_switch_true_tone_default(p, scratch, data_mount);"),
              1u);
    EXPECT_EQ(Count(twentySeven, "<key>CBColorAdaptationEnabled</key>"), 0u) << "no once-only seed: the key goes in "
                                                                                "wherever it is missing";
}
}  // namespace
