// kernel/switch/boot/prefs.c against CoreFoundation's own binary plist writer and reader (the code cfprefsd and
// SpringBoard run), and the guard that no boot-time setup sets SpringBoard's never-dim keys.
#include <gtest/gtest.h>

#include <CoreFoundation/CoreFoundation.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

extern "C" {
#include <switch/prefs.h>
}

namespace {
using Bytes = std::vector<uint8_t>;

const char *const kNeverDim[] = {"SBDisableAutoDim", "SBDontDimOrLockOnAC"};

template <typename T> struct Owned {
    T ref;

    explicit Owned(T r) : ref(r)
    {}

    ~Owned()
    {
        if (ref)
            CFRelease(ref);
    }

    Owned(const Owned &) = delete;
    Owned &operator=(const Owned &) = delete;
};

CFStringRef Str(const char *text)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, text, kCFStringEncodingUTF8);
}

void Set(CFMutableDictionaryRef dict, const char *key, CFTypeRef value)
{
    Owned<CFStringRef> k(Str(key));
    CFDictionarySetValue(dict, k.ref, value);
}

CFMutableDictionaryRef NewDict()
{
    return CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                     &kCFTypeDictionaryValueCallBacks);
}

Bytes Binary(CFPropertyListRef plist)
{
    Owned<CFDataRef> data(
        CFPropertyListCreateData(kCFAllocatorDefault, plist, kCFPropertyListBinaryFormat_v1_0, 0, nullptr));
    EXPECT_NE(data.ref, nullptr);
    const uint8_t *bytes = CFDataGetBytePtr(data.ref);
    return Bytes(bytes, bytes + CFDataGetLength(data.ref));
}

CFPropertyListRef Parse(const Bytes &bytes)
{
    Owned<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, bytes.data(), (CFIndex)bytes.size()));
    return CFPropertyListCreateWithData(kCFAllocatorDefault, data.ref, kCFPropertyListImmutable, nullptr, nullptr);
}

switch_prefs_result Drop(Bytes &bytes, size_t *length = nullptr)
{
    size_t n = bytes.size();
    const switch_prefs_result result = switch_prefs_drop_keys(bytes.data(), &n, kNeverDim, 2);
    if (result == SWITCH_PREFS_DROPPED)
        bytes.resize(n);
    if (length)
        *length = n;
    return result;
}

// SpringBoard's user plist as earlier builds left it: our keys among SpringBoard's own (brightness, a nested
// dictionary, an array, data, a date), every boolean true one shared object.
CFMutableDictionaryRef SpringBoardPlist(int extra)
{
    CFMutableDictionaryRef dict = NewDict();
    Set(dict, "BKNoWatchdogs", kCFBooleanTrue);
    Set(dict, "SBDisableAutoDim", kCFBooleanTrue);
    Set(dict, "SBDontDimOrLockOnAC", kCFBooleanTrue);
    Set(dict, "SBDisableIdleSleep", kCFBooleanTrue);
    const double level = 0.42;
    Owned<CFNumberRef> brightness(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &level));
    Set(dict, "SBBacklightLevel2", brightness.ref);
    Owned<CFMutableDictionaryRef> exceptions(NewDict());
    const double seconds = 100;
    Owned<CFNumberRef> limit(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &seconds));
    Set(exceptions.ref, "com.apple.purplebuddy", limit.ref);
    Set(dict, "FBLaunchWatchdogExceptions", exceptions.ref);
    Owned<CFStringRef> name(Str("SBDisableAutoDim"));  // a value spelled like a key stays
    Set(dict, "SBLastAppName", name.ref);
    const void *items[] = {kCFBooleanFalse, name.ref};
    Owned<CFArrayRef> array(CFArrayCreate(kCFAllocatorDefault, items, 2, &kCFTypeArrayCallBacks));
    Set(dict, "SBRecentItems", array.ref);
    const uint8_t blob[] = {0xde, 0xad, 0xbe, 0xef};
    Owned<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, blob, sizeof(blob)));
    Set(dict, "SBBlob", data.ref);
    Owned<CFDateRef> date(CFDateCreate(kCFAllocatorDefault, 800000000.0));
    Set(dict, "SBLastBoot", date.ref);
    for (int i = 0; i < extra; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "SBExtraKey%04d", i);
        Owned<CFNumberRef> n(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &i));
        Set(dict, key, n.ref);
    }
    return dict;
}

void ExpectDroppedRoundTrip(int extra)
{
    Owned<CFMutableDictionaryRef> before(SpringBoardPlist(extra));
    Bytes bytes = Binary(before.ref);
    const size_t length = bytes.size();
    ASSERT_EQ(Drop(bytes), SWITCH_PREFS_DROPPED);
    EXPECT_EQ(bytes.size(), length);

    Owned<CFPropertyListRef> after(Parse(bytes));
    ASSERT_NE(after.ref, nullptr) << "CoreFoundation cannot read the edited plist";
    ASSERT_EQ(CFGetTypeID(after.ref), CFDictionaryGetTypeID());
    Owned<CFMutableDictionaryRef> expected(CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, before.ref));
    for (const char *key : kNeverDim) {
        Owned<CFStringRef> k(Str(key));
        CFDictionaryRemoveValue(expected.ref, k.ref);
    }
    EXPECT_TRUE(CFEqual(after.ref, expected.ref));

    size_t again = bytes.size();
    EXPECT_EQ(switch_prefs_drop_keys(bytes.data(), &again, kNeverDim, 2), SWITCH_PREFS_UNCHANGED);
}

TEST(SwitchPrefs, BinaryKeepsEverythingElse)
{
    ExpectDroppedRoundTrip(0);
}

TEST(SwitchPrefs, BinaryLongDictionaryStaysLong)
{
    ExpectDroppedRoundTrip(30);  // 41 entries, 39 kept: the 0xDF count form both times
}

TEST(SwitchPrefs, BinaryLongDictionaryBecomesShort)
{
    ExpectDroppedRoundTrip(5);  // 16 entries (0xDF form), 14 kept (0xDE)
}

TEST(SwitchPrefs, BinaryWideReferences)
{
    ExpectDroppedRoundTrip(400);  // over 255 objects: two-byte object references
}

TEST(SwitchPrefs, BinaryWithoutTheKeysIsUntouched)
{
    Owned<CFMutableDictionaryRef> dict(NewDict());
    Set(dict.ref, "SBBacklightLevel2", kCFBooleanTrue);
    Bytes bytes = Binary(dict.ref);
    const Bytes original = bytes;
    EXPECT_EQ(Drop(bytes), SWITCH_PREFS_UNCHANGED);
    EXPECT_EQ(bytes, original);
}

TEST(SwitchPrefs, BinaryOfOnlyTheKeysIsEmpty)
{
    Owned<CFMutableDictionaryRef> dict(NewDict());
    Set(dict.ref, "SBDisableAutoDim", kCFBooleanTrue);
    Set(dict.ref, "SBDontDimOrLockOnAC", kCFBooleanFalse);
    Bytes bytes = Binary(dict.ref);
    EXPECT_EQ(Drop(bytes), SWITCH_PREFS_EMPTY);
}

TEST(SwitchPrefs, BinaryTopLevelArrayIsUnreadable)
{
    const void *items[] = {kCFBooleanTrue};
    Owned<CFArrayRef> array(CFArrayCreate(kCFAllocatorDefault, items, 1, &kCFTypeArrayCallBacks));
    Bytes bytes = Binary(array.ref);
    EXPECT_EQ(Drop(bytes), SWITCH_PREFS_UNREADABLE);
}

// The edit rewrites the dictionary's own bytes, so an offset table that puts another object among them (damage) leaves
// the file alone: CoreFoundation would read that object from the rewritten bytes.
TEST(SwitchPrefs, ObjectInsideTheDictionaryIsUnreadable)
{
    Owned<CFMutableDictionaryRef> dict(SpringBoardPlist(0));
    Bytes bytes = Binary(dict.ref);
    const uint8_t *trailer = bytes.data() + bytes.size() - 32;
    const size_t offsetSize = trailer[6];
    uint64_t top = 0, table = 0;
    for (int i = 0; i < 8; ++i) {
        top = top << 8 | trailer[16 + i];
        table = table << 8 | trailer[24 + i];
    }
    const auto offsetAt = [&](uint64_t object) {
        uint64_t offset = 0;
        for (size_t i = 0; i < offsetSize; ++i)
            offset = offset << 8 | bytes[table + object * offsetSize + i];
        return offset;
    };
    uint64_t inside = offsetAt(top) + 3;
    const size_t victim = top ? 0 : 1;
    for (size_t i = offsetSize; i-- > 0; inside >>= 8)
        bytes[table + victim * offsetSize + i] = (uint8_t)inside;
    EXPECT_EQ(Drop(bytes), SWITCH_PREFS_UNREADABLE);
}

// Damaged files: whatever comes back, nothing is read or written outside the buffer (AddressSanitizer), and an edit of
// a plist CoreFoundation still reads is one it reads too.
TEST(SwitchPrefs, DamagedBinaryStaysInBounds)
{
    Owned<CFMutableDictionaryRef> before(SpringBoardPlist(20));
    const Bytes good = Binary(before.ref);
    std::mt19937 rng(1107);
    for (size_t cut = 0; cut < good.size(); ++cut) {
        Bytes bytes(good.begin(), good.begin() + (long)cut);
        (void)Drop(bytes);
    }
    for (int round = 0; round < 4000; ++round) {
        Bytes bytes = good;
        for (int flips = 1 + round % 4; flips > 0; --flips)
            bytes[rng() % bytes.size()] ^= (uint8_t)(1u << (rng() % 8));
        Owned<CFPropertyListRef> damaged(Parse(bytes));
        if (Drop(bytes) == SWITCH_PREFS_DROPPED && damaged.ref) {
            Owned<CFPropertyListRef> after(Parse(bytes));
            EXPECT_NE(after.ref, nullptr) << "round " << round;
        }
    }
}

const char kXmlHead[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>";
const char kXmlTail[] = "</dict></plist>\n";

Bytes Xml(const std::string &body)
{
    const std::string text = kXmlHead + body + kXmlTail;
    return Bytes(text.begin(), text.end());
}

TEST(SwitchPrefs, XmlAsEarlierKernelsWroteIt)
{
    Bytes bytes =
        Xml("<key>BKNoWatchdogs</key><true/>"
            "<key>FBLaunchWatchdogExceptions</key><dict><key>com.apple.purplebuddy</key><real>100</real></dict>"
            "<key>MCForceShowCloudConfigurationUI</key><false/><key>SBDisableAutoDim</key><true/>"
            "<key>SBDontDimOrLockOnAC</key><true/><key>SBDisableIdleSleep</key><true/>"
            "<key>SBHomeLargeIcons</key><true/>");
    ASSERT_EQ(Drop(bytes), SWITCH_PREFS_DROPPED);
    EXPECT_EQ(bytes, Xml("<key>BKNoWatchdogs</key><true/>"
                         "<key>FBLaunchWatchdogExceptions</key><dict><key>com.apple.purplebuddy</key><real>100</real>"
                         "</dict><key>MCForceShowCloudConfigurationUI</key><false/><key>SBDisableIdleSleep</key><true/>"
                         "<key>SBHomeLargeIcons</key><true/>"));
    Owned<CFPropertyListRef> after(Parse(bytes));
    ASSERT_NE(after.ref, nullptr);
    EXPECT_EQ(CFDictionaryGetCount((CFDictionaryRef)after.ref), 5);
}

TEST(SwitchPrefs, XmlOfOnlyTheKeysIsEmpty)
{
    Bytes bytes = Xml("<key>SBDisableAutoDim</key><true/><key>SBDontDimOrLockOnAC</key><false/>");
    EXPECT_EQ(Drop(bytes), SWITCH_PREFS_EMPTY);
}

TEST(SwitchPrefs, XmlWithAnotherValueIsLeftAlone)
{
    Bytes bytes = Xml("<key>SBDisableAutoDim</key><integer>1</integer><key>BKNoWatchdogs</key><true/>");
    EXPECT_EQ(Drop(bytes), SWITCH_PREFS_UNREADABLE);
}

TEST(SwitchPrefs, NotAPlist)
{
    Bytes bytes = {'h', 'e', 'l', 'l', 'o'};
    EXPECT_EQ(Drop(bytes), SWITCH_PREFS_UNREADABLE);
}

// The guard: SpringBoard's Auto-Lock is the user's (11-07). No boot-time setup writes SBDisableAutoDim or
// SBDontDimOrLockOnAC, in either kernel or in the 12377 port record; the names appear only in the one function that
// takes them out of a user plist earlier builds wrote, the same in both kernels and called from the setup.
const char *const kKernels[] = {"../xnu-10063.141.1/bsd/kern/kern_exec.c", "../xnu-12377.121.6/bsd/kern/kern_exec.c"};
const char kRelease[] = "native_switch_never_dim_release(proc_t p, user_addr_t scratch, mount_t data_mount)";

TEST(SwitchPrefsGuard, NoSetupSetsNeverDim)
{
    using switch_ios::test::Body;
    using switch_ios::test::Count;
    using switch_ios::test::SourceText;
    std::string release;
    for (const char *path : kKernels) {
        const std::string source = SourceText(path);
        const std::string body = Body(source, kRelease);
        ASSERT_FALSE(body.empty()) << path;
        for (const char *key : kNeverDim) {
            const std::string literal = std::string("\"") + key + "\"";
            EXPECT_EQ(Count(source, std::string("<key>") + key + "</key>"), 0u) << path << " writes " << key;
            EXPECT_EQ(Count(body, literal), 1u) << path;
            EXPECT_EQ(Count(source, literal), 1u) << path << ": " << key << " outside the release";
        }
        EXPECT_EQ(Count(Body(source, "native_switch_setup_state(proc_t p, user_addr_t scratch, mount_t data_mount)"),
                        "native_switch_never_dim_release(p, scratch, data_mount);"),
                  1u)
            << path;
        if (release.empty())
            release = body;
        EXPECT_EQ(body, release) << path << ": the two kernels' releases differ";
    }
    const std::string record = SourceText("../../translate/arm64e/xnu/port-12377/switch-on-12377.diff");
    ASSERT_FALSE(record.empty());
    for (const char *key : kNeverDim)
        EXPECT_EQ(Count(record, key), 0u) << "the 12377 port record still sets " << key;
}
}  // namespace
