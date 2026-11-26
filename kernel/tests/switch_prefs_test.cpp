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

// The setup's com.apple.purplebuddy as kern_exec.c writes it, and the domain as cfprefsd saves it once the OS has set
// keys of its own there: ours hold, so the boot leaves the file alone.
const char kPurpleBuddy[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>"
                            "<key>SetupDone</key><true/><key>SetupFinishedAllSteps</key><true/>"
                            "<key>SetupVersion</key><integer>11</integer><key>daemonizeNeedsToRun</key><false/>"
                            "</dict></plist>\n";

bool Holds(const Bytes &plist)
{
    return switch_prefs_holds(plist.data(), plist.size(), kPurpleBuddy, sizeof(kPurpleBuddy) - 1);
}

CFMutableDictionaryRef PurpleBuddy(int version, bool daemonize)
{
    CFMutableDictionaryRef dict = NewDict();
    Set(dict, "SetupDone", kCFBooleanTrue);
    Set(dict, "SetupFinishedAllSteps", kCFBooleanTrue);
    Owned<CFNumberRef> number(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &version));
    Set(dict, "SetupVersion", number.ref);
    Set(dict, "daemonizeNeedsToRun", daemonize ? kCFBooleanTrue : kCFBooleanFalse);
    return dict;
}

Bytes AsXml(CFPropertyListRef plist)
{
    Owned<CFDataRef> data(
        CFPropertyListCreateData(kCFAllocatorDefault, plist, kCFPropertyListXMLFormat_v1_0, 0, nullptr));
    const uint8_t *bytes = CFDataGetBytePtr(data.ref);
    return Bytes(bytes, bytes + CFDataGetLength(data.ref));
}

TEST(SwitchPrefsHolds, OurOwnFileHolds)
{
    EXPECT_TRUE(Holds(Bytes(kPurpleBuddy, kPurpleBuddy + sizeof(kPurpleBuddy) - 1)));
}

TEST(SwitchPrefsHolds, BinaryWithTheOsKeysBesideOursHolds)
{
    Owned<CFMutableDictionaryRef> dict(PurpleBuddy(11, false));
    Owned<CFStringRef> country(Str("GB"));
    Set(dict.ref, "GuessedCountry", country.ref);
    Owned<CFDateRef> date(CFDateCreate(kCFAllocatorDefault, 800000000.0));
    Set(dict.ref, "SetupLastExit", date.ref);
    const int big = 70000;
    Owned<CFNumberRef> number(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &big));
    Set(dict.ref, "PBAppActivity", number.ref);
    EXPECT_TRUE(Holds(Binary(dict.ref)));
    EXPECT_TRUE(Holds(AsXml(dict.ref)));
}

TEST(SwitchPrefsHolds, AChangedOrMissingKeyDoesNot)
{
    Owned<CFMutableDictionaryRef> older(PurpleBuddy(10, false));
    EXPECT_FALSE(Holds(Binary(older.ref)));
    EXPECT_FALSE(Holds(AsXml(older.ref)));
    Owned<CFMutableDictionaryRef> flipped(PurpleBuddy(11, true));
    EXPECT_FALSE(Holds(Binary(flipped.ref)));
    Owned<CFMutableDictionaryRef> missing(PurpleBuddy(11, false));
    Owned<CFStringRef> key(Str("SetupDone"));
    CFDictionaryRemoveValue(missing.ref, key.ref);
    EXPECT_FALSE(Holds(Binary(missing.ref)));
    EXPECT_FALSE(Holds(AsXml(missing.ref)));
}

TEST(SwitchPrefsHolds, AValueOfAnotherKindDoesNot)
{
    Owned<CFMutableDictionaryRef> dict(PurpleBuddy(11, false));
    Set(dict.ref, "SetupDone", kCFBooleanFalse);
    Owned<CFStringRef> text(Str("11"));
    Set(dict.ref, "SetupVersion", text.ref);
    EXPECT_FALSE(Holds(Binary(dict.ref)));
    Owned<CFMutableDictionaryRef> real(PurpleBuddy(11, false));
    const double eleven = 11;
    Owned<CFNumberRef> number(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &eleven));
    Set(real.ref, "SetupVersion", number.ref);
    EXPECT_FALSE(Holds(Binary(real.ref)));
}

TEST(SwitchPrefsHolds, NothingToHoldOrNotAPlist)
{
    const Bytes ours(kPurpleBuddy, kPurpleBuddy + sizeof(kPurpleBuddy) - 1);
    const std::string nothing = std::string(kXmlHead) + kXmlTail;
    EXPECT_FALSE(switch_prefs_holds(ours.data(), ours.size(), nothing.data(), nothing.size()));
    const std::string string_value = std::string(kXmlHead) + "<key>SetupDone</key><string>yes</string>" + kXmlTail;
    EXPECT_FALSE(switch_prefs_holds(ours.data(), ours.size(), string_value.data(), string_value.size()));
    const Bytes hello = {'h', 'e', 'l', 'l', 'o'};
    EXPECT_FALSE(Holds(hello));
    Owned<CFMutableDictionaryRef> dict(PurpleBuddy(11, false));
    Bytes truncated = Binary(dict.ref);
    truncated.resize(truncated.size() / 2);
    EXPECT_FALSE(Holds(truncated));
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

// KERN-46: BackBoard's watchdogs on SpringBoard are Apple's. No setup writes BKNoWatchdogs into the managed plist, and
// the one-time release takes the key earlier kernels wrote out of the user's plist, called from the setup in both trees
TEST(SwitchPrefsGuard, NoSetupTurnsBackBoardsWatchdogsOff)
{
    using switch_ios::test::Body;
    using switch_ios::test::Count;
    using switch_ios::test::SourceText;
    for (const char *path : kKernels) {
        const std::string source = SourceText(path);
        EXPECT_EQ(Count(source, "<key>BKNoWatchdogs</key>"), 0u) << path;
        const std::string body =
            Body(source, "native_switch_watchdogs_release(proc_t p, user_addr_t scratch, mount_t data_mount)");
        EXPECT_EQ(Count(body, "\"BKNoWatchdogs\""), 1u) << path;
        EXPECT_EQ(Count(Body(source, "native_switch_setup_state(proc_t p, user_addr_t scratch, mount_t data_mount)"),
                        "native_switch_watchdogs_release(p, scratch, data_mount);"),
                  1u)
            << path;
    }
}

// SetupVersion is the selected IPSW's (<switch/setup_state.h>, one header per build), in both kernels: a number written
// into the setup's purplebuddy body would apply to every build.
TEST(SwitchPrefsGuard, SetupVersionComesFromTheIpswTable)
{
    using switch_ios::test::Body;
    using switch_ios::test::SourceText;
    const std::string key = "<key>SetupVersion</key><integer>", table = "\" SWITCH_SETUP_VERSION \"";
    for (const char *path : kKernels) {
        const std::string setup =
            Body(SourceText(path), "native_switch_setup_state(proc_t p, user_addr_t scratch, mount_t data_mount)");
        const size_t at = setup.find(key);
        ASSERT_NE(at, std::string::npos) << path;
        EXPECT_EQ(setup.compare(at + key.size(), table.size(), table), 0) << path << ": " << setup.substr(at, 60);
        EXPECT_EQ(setup.find(key, at + 1), std::string::npos) << path;
    }
}

// The setup's sync decisions (<switch/boot_sync.h>, kernel/tests/switch_boot_sync_test.cpp) as both kernels apply them:
// REDONE at the changes the next boot remakes (IconServices' drop — top-level unlink on 10063, trash mkdir and
// rename-aside on 12377 — and its marker, timed's drop and its marker), every other write DURABLE, the class deciding
// the write's IO_SYNC, and the step's end syncing through switch_boot_sync_finish. A change classed wrongly would ship
// unsynced with every host test green.
TEST(SwitchPrefsGuard, OnlyTheRemadeChangesGoOutUnsynced)
{
    using switch_ios::test::Body;
    using switch_ios::test::Count;
    using switch_ios::test::SourceText;
    const char *const functions[] = {
        "\nnative_switch_watchdog_directory(", "\nnative_switch_replace_file(", "\nnative_switch_data_file(",
        "\nnative_switch_watchdog_file(",      "\nnative_switch_unlink(",       "\nnative_switch_mkdir(",
        "\nnative_switch_icon_cache_write(",   "\nnative_switch_timed_cache(",  "\nnative_switch_sync_volume(",
    };
    std::vector<std::string> first;
    for (const char *path : kKernels) {
        const std::string source = SourceText(path);
        const bool salted = std::string(path).find("12377") != std::string::npos;
        EXPECT_EQ(Count(source, "SWITCH_BOOT_REDONE"), salted ? 5u : 4u) << path;
        EXPECT_EQ(Count(Body(source, "\nnative_switch_icon_cache_write("), "SWITCH_BOOT_REDONE"), 1u) << path;
        if (salted) {
            EXPECT_EQ(Count(Body(source, "\nnative_switch_icon_cache_mkdir("), "SWITCH_BOOT_REDONE"), 1u) << path;
            EXPECT_EQ(Count(Body(source, "\nnative_switch_icon_cache_rename("), "SWITCH_BOOT_REDONE"), 1u) << path;
        } else {
            EXPECT_EQ(Count(Body(source, "\nnative_switch_icon_cache_unlink("), "SWITCH_BOOT_REDONE"), 1u) << path;
        }
        EXPECT_EQ(Count(Body(source, "\nnative_switch_timed_cache("), "SWITCH_BOOT_REDONE"), 2u) << path;
        EXPECT_EQ(Count(Body(source, "\nnative_switch_watchdog_file("), "mode, SWITCH_BOOT_DURABLE);"), 1u) << path;
        const std::string replace = Body(source, "\nnative_switch_replace_file(");
        EXPECT_EQ(
            Count(replace, "const int io = switch_boot_sync_change(&native_switch_sync, change, path) ? IO_SYNC : 0;"),
            1u)
            << path;
        EXPECT_EQ(Count(replace, "UIO_SYSSPACE, io, vfs_context_ucred(context)"), 1u) << path;
        EXPECT_EQ(Count(Body(source, "\nnative_switch_unlink("),
                        "switch_boot_sync_change(&native_switch_sync, change, path)"),
                  1u)
            << path;
        EXPECT_EQ(Count(Body(source, "\nnative_switch_mkdir("), "SWITCH_BOOT_DURABLE, path);"), 1u) << path;
        const std::string directory = Body(source, "\nnative_switch_watchdog_directory(");
        EXPECT_EQ(Count(directory, "SWITCH_BOOT_DURABLE, path);"), 1u) << path;
        EXPECT_EQ(Count(directory, "if (!error && switch_boot_sync_due(&native_switch_sync)) error = VNOP_FSYNC("), 1u)
            << path;
        EXPECT_EQ(Count(Body(source, "\nnative_switch_sync_volume("), "return VNOP_FSYNC("), 1u) << path;
        const std::string policy = Body(source, "\nnative_switch_watchdog_policy(");
        EXPECT_EQ(Count(policy, ".sync_volume = native_switch_sync_volume"), 1u) << path;
        EXPECT_EQ(Count(policy, "error = switch_boot_sync_finish(&native_switch_sync, error, &finish);"), 1u) << path;
        EXPECT_EQ(Count(policy, "VNOP_FSYNC("), 0u) << path << ": the volume sync is switch_boot_sync_finish's";
        std::vector<std::string> bodies;
        for (const char *function : functions)
            bodies.push_back(Body(source, function));
        if (first.empty())
            first = bodies;
        EXPECT_EQ(bodies, first) << path << ": the two kernels' sync decisions differ";
    }
}

// timed's cache is kept only when it holds the boot session UUID of this clock's last boot, the same in both kernels
// (drivers/SwitchPower/SwitchMonotonicBase.h): one an older kernel's boot saved would put the time decades ahead.
TEST(SwitchPrefsGuard, TimedCacheKeptOnlyFromThisClock)
{
    using switch_ios::test::Body;
    using switch_ios::test::Count;
    using switch_ios::test::SourceText;
    std::string first;
    for (const char *path : kKernels) {
        const std::string source = SourceText(path);
        const std::string body =
            Body(source, "native_switch_timed_cache(proc_t p, user_addr_t scratch, mount_t data_mount)");
        ASSERT_FALSE(body.empty()) << path;
        EXPECT_EQ(Count(body, "\"/private/var/db/timed/com.apple.timed.plist\""), 1u) << path;
        EXPECT_EQ(Count(body, "keep = native_switch_bytes_hold(cached, cached_length, last, last_length);"), 1u)
            << path;
        EXPECT_LT(body.find("if (!keep) {"), body.find("native_switch_unlink(p, scratch, cache, SWITCH_BOOT_REDONE)"))
            << path;
        EXPECT_LT(body.find("native_switch_unlink(p, scratch, cache, SWITCH_BOOT_REDONE)"),
                  body.find("native_switch_data_file(p, scratch, marker, bootsessionuuid_string"))
            << path << ": the marker is this boot's, written after the drop";
        EXPECT_EQ(Count(Body(source, "native_switch_setup_state(proc_t p, user_addr_t scratch, mount_t data_mount)"),
                        "native_switch_timed_cache(p, scratch, data_mount);"),
                  1u)
            << path;
        if (first.empty())
            first = body;
        EXPECT_EQ(body, first) << path << ": the two kernels' drops differ";
    }
}

TEST(SwitchPrefsGuard, DiskArbitrationEnablesInternalUserFSMountInBothKernels)
{
    using switch_ios::test::Body;
    using switch_ios::test::Count;
    using switch_ios::test::SourceText;
    for (const char *path : kKernels) {
        const std::string setup =
            Body(SourceText(path), "native_switch_setup_state(proc_t p, user_addr_t scratch, mount_t data_mount)");
        EXPECT_EQ(Count(setup, "\"/private/var/preferences/SystemConfiguration\""), 1u) << path;
        EXPECT_EQ(Count(setup, "\"/private/var/preferences/SystemConfiguration/"
                               "com.apple.DiskArbitration.diskarbitrationd.plist\""),
                  1u)
            << path;
        EXPECT_EQ(Count(setup, "<key>DAEnableUserFSMountInternal</key><true/>"), 1u) << path;
    }
}
}  // namespace
