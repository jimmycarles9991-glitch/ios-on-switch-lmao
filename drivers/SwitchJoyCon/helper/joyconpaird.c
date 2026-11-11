// SPDX-License-Identifier: BSD-2-Clause-Patent
// joyconpaird: hands Joy-Con pairings to bluetoothd, so a Joy-Con pulled off the rail reconnects over Bluetooth.
//
// Two sources, both {address, name, link key (16 bytes, HCI order)}:
//   * the rail driver (../SwitchJoyConRail.cpp), which pairs a Joy-Con to the console's Bluetooth address over the
//     rail (or finds it already paired) and publishes one SwitchJoyConPairing registry entry per Joy-Con:
//     AddressString ("xx:xx:xx:xx:xx:xx"), LinkKey, Name, ClassOfDevice;
//   * hekate's "Dump Joy-Con BT" of the pairings Horizon made (switchroot/joycon_mac.ini), which the deploy tool stages
//     as kStagedPath (tools/deploy/joycon_pairing.py: HostAddress, Controllers [{Address, Name, ProductID, LinkKey}]);
//     the kernel gives the controller that HostAddress (../../SwitchBluetooth/SwitchBluetooth.cpp), so those
//     Joy-Cons reconnect without ever touching the rail. A dump is imported once (kImportedPath keeps a copy of the
//     one imported): bluetoothd keeps the keys, and a later boot must not put back a key the rail has replaced since.
//     A Joy-Con whose rail record is present is taken from the rail (it was just paired there).
// bluetoothd keeps classic link keys in the system keychain (generic password, service "MobileBluetooth", account =
// address, access group com.apple.bluetooth: BTKCSetData in bluetoothd) and the device records in the
// com.apple.MobileBluetooth.devices preferences domain. Neither is reachable from the kernel, and writing them behind
// bluetoothd's back would race its caches; MobileBluetooth.framework has the call Apple uses for accessories paired
// over a wire instead:
//   BTAccessoryManagerSetLinkKeyEx(manager, address[6], name, classOfDevice, serviceMask, linkKey[16], &device)
// (XPC kCBMsgIdAccessorySetLinkKeyExMsg; bluetoothd BTAccessoryMsgHandler::handleBTAccessorySetLinkKeyExMsg creates
// the paired device and stores the key itself). The address bytes come from BTDeviceAddressFromString, so they are in
// bluetoothd's own order. Service mask 0x20 is "ServiceHID" in bluetoothd's record writer. 21H461 bluetoothd
// FUN_10053dc80 -> FUN_1003de5ec: finds or creates the device by address, sets class of device, name and the service
// bits, and stores the 16 bytes as they come as an authenticated combination key (type 5; FUN_100555d1c(key, 5),
// FUN_10056a990), so importing the same key again changes nothing.
//
// On iOS 17 the BlueTool stand-in starts it at each of bluetoothd's chip boots (../../SwitchBluetooth/BlueTool/BlueTool.c:
// launchd does not load LaunchDaemons plists from the Data overlay). On iOS 27, whose stand-in is sandboxed, launchd
// starts it at boot from com.brunostjohn.switch.joyconpaird.plist, which the overlay shows in the Supplemental cryptex's
// LaunchDaemons (overlay/manifest.json), and again after a failing exit. Either way it runs as mobile, outside any
// sandbox, with com.apple.bluetooth.system/.internal (what sharingd holds for MobileBluetooth's accessory calls). One
// instance runs (kLockPath); it imports the dump and every rail record present, then waits for new rail records for the
// boot, or until bluetoothd's session ends. MobileBluetooth is private: it is loaded with dlopen,
// and the prototypes below are from its disassembly (21H461, BTAccessoryManagerSetLinkKeyEx: x0 manager, x1 address
// bytes, x2 name, w3 class, w4 service mask, x5 key, x6 device out).
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <os/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xpc/xpc.h>

#include "joycon_pairing_import.h"

typedef void *BTSession;
typedef void *BTAccessoryManager;
typedef void *BTDevice;

typedef struct {
    uint8_t bytes[6];
} BTDeviceAddress;

typedef void (*BTSessionEventCallback)(BTSession session, int event, int result, void *context);

// Event 0 with result 0 = attached, 1 detached, 2 terminated, 3 failed (GameController's JoyConHIDServicePlugin).
typedef struct {
    BTSessionEventCallback sessionEvent;
    void *reserved[3];
} BTSessionCallbacks;

static int (*BTSessionAttachWithQueue)(const char *name, const BTSessionCallbacks *callbacks, void *context,
                                       dispatch_queue_t queue);
static int (*BTAccessoryManagerGetDefault)(BTSession session, BTAccessoryManager *manager);
static int (*BTDeviceAddressFromString)(const char *string, BTDeviceAddress *address);
static int (*BTAccessoryManagerSetLinkKeyEx)(BTAccessoryManager manager, const BTDeviceAddress *address,
                                             const char *name, uint32_t classOfDevice, uint32_t serviceMask,
                                             const uint8_t key[16], BTDevice *device);

enum {
    kServiceHID = 0x20
};

static const char kStagedPath[] = "/private/var/Switch/bluetooth/joycon-pairing.plist";  // deploy's, read-only here
static const char kImportedPath[] = "/private/var/mobile/Library/com.brunostjohn.switch.joycon-pairing.imported.plist";
static const char kLockPath[] = "/private/var/tmp/com.brunostjohn.switch.joyconpaird.lock";

static dispatch_queue_t queue;
static BTAccessoryManager manager;
static IONotificationPortRef port;
static io_iterator_t records;
static os_log_t logger;

static CFTypeRef property(io_service_t service, const char *key, CFTypeID type)
{
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef value = name ? IORegistryEntryCreateCFProperty(service, name, kCFAllocatorDefault, 0) : NULL;
    if (name)
        CFRelease(name);
    if (value && CFGetTypeID(value) != type) {
        CFRelease(value);
        value = NULL;
    }
    return value;
}

// One pairing to bluetoothd; returns 0 or its error. `source` is for the log line.
static int pair(const char *address, const char *name, uint32_t classOfDevice, const uint8_t key[16],
                const char *source)
{
    BTDeviceAddress bytes;
    BTDevice device = NULL;
    int result = BTDeviceAddressFromString(address, &bytes);
    if (!result)
        result = BTAccessoryManagerSetLinkKeyEx(manager, &bytes, name, classOfDevice, kServiceHID, key, &device);
    if (result)
        os_log_error(logger, "joyconpaird: %{public}s (%{public}s, %{public}s) not imported: %d", name, address, source,
                     result);
    else
        os_log(logger, "joyconpaird: %{public}s (%{public}s) paired with bluetoothd (%{public}s)", name, address,
               source);
    return result;
}

static void import(io_service_t service)
{
    CFStringRef addressString = property(service, "AddressString", CFStringGetTypeID());
    CFStringRef nameString = property(service, "Name", CFStringGetTypeID());
    CFStringRef sourceString = property(service, "Source", CFStringGetTypeID());
    CFDataRef key = property(service, "LinkKey", CFDataGetTypeID());
    CFNumberRef cod = property(service, "ClassOfDevice", CFNumberGetTypeID());
    char address[18] = "", name[32] = "Joy-Con", source[16] = "?";
    uint32_t classOfDevice = 0x000508;
    if (nameString)
        CFStringGetCString(nameString, name, sizeof(name), kCFStringEncodingUTF8);
    if (sourceString)
        CFStringGetCString(sourceString, source, sizeof(source), kCFStringEncodingUTF8);
    if (cod)
        CFNumberGetValue(cod, kCFNumberSInt32Type, &classOfDevice);
    char from[32];
    snprintf(from, sizeof(from), "rail, %s", source);
    if (addressString && key && CFDataGetLength(key) == 16 &&
        CFStringGetCString(addressString, address, sizeof(address), kCFStringEncodingUTF8)) {
        pair(address, name, classOfDevice, CFDataGetBytePtr(key), from);
    } else {
        os_log_error(logger, "joyconpaird: a SwitchJoyConPairing record without address or 16-byte key");
    }
    if (addressString)
        CFRelease(addressString);
    if (nameString)
        CFRelease(nameString);
    if (sourceString)
        CFRelease(sourceString);
    if (key)
        CFRelease(key);
    if (cod)
        CFRelease(cod);
}

static void matched(void *context, io_iterator_t iterator)
{
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        import(service);
        IOObjectRelease(service);
    }
}

// Whether the rail driver has a record for `address` now (it wins over the dump; see the file comment).
static bool railHas(const char *address)
{
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("SwitchJoyConPairing"), &iterator) !=
        KERN_SUCCESS)
        return false;
    bool found = false;
    io_service_t service;
    while (!found && (service = IOIteratorNext(iterator))) {
        CFStringRef string = property(service, "AddressString", CFStringGetTypeID());
        char other[18] = "";
        found = string && CFStringGetCString(string, other, sizeof(other), kCFStringEncodingUTF8) &&
                !strcasecmp(other, address);
        if (string)
            CFRelease(string);
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return found;
}

static CFDataRef readFile(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    struct stat st;
    CFMutableDataRef data = NULL;
    if (!fstat(fd, &st) && st.st_size > 0 && st.st_size < 65536) {
        data = CFDataCreateMutable(kCFAllocatorDefault, st.st_size);
        CFDataSetLength(data, st.st_size);
        if (read(fd, CFDataGetMutableBytePtr(data), st.st_size) != st.st_size) {
            CFRelease(data);
            data = NULL;
        }
    }
    close(fd);
    return data;
}

static bool writeFile(const char *path, CFDataRef data)
{
    char partial[PATH_MAX];
    snprintf(partial, sizeof(partial), "%s.partial", path);
    int fd = open(partial, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0)
        return false;
    const bool ok = write(fd, CFDataGetBytePtr(data), CFDataGetLength(data)) == CFDataGetLength(data) && !fsync(fd);
    close(fd);
    return ok && !rename(partial, path);
}

// The staged dump, once per dump (see the file comment).
static void importStaged(void)
{
    CFDataRef staged = readFile(kStagedPath);
    if (!staged)
        return;
    CFDataRef copy = readFile(kImportedPath);
    const bool imported = copy && CFEqual(staged, copy);
    if (copy)
        CFRelease(copy);
    CFPropertyListRef plist =
        imported ? NULL
                 : CFPropertyListCreateWithData(kCFAllocatorDefault, staged, kCFPropertyListImmutable, NULL, NULL);
    CFArrayRef controllers = plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()
                                 ? CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("Controllers"))
                                 : NULL;
    if (controllers && CFGetTypeID(controllers) != CFArrayGetTypeID())
        controllers = NULL;
    struct switch_joycon_dump_import import = switch_joycon_dump_begin(imported, controllers != NULL);
    if (switch_joycon_dump_unreadable(&import))
        os_log_error(logger, "joyconpaird: %{public}s is not a pairing dump", kStagedPath);
    for (CFIndex i = 0; switch_joycon_dump_pending(&import) && i < CFArrayGetCount(controllers); ++i) {
        CFDictionaryRef entry = CFArrayGetValueAtIndex(controllers, i);
        CFStringRef addressString =
            CFGetTypeID(entry) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(entry, CFSTR("Address")) : NULL;
        CFStringRef nameString = addressString ? CFDictionaryGetValue(entry, CFSTR("Name")) : NULL;
        CFDataRef key = addressString ? CFDictionaryGetValue(entry, CFSTR("LinkKey")) : NULL;
        char address[18] = "", name[32] = "Joy-Con";
        const bool wellFormed = addressString && CFGetTypeID(addressString) == CFStringGetTypeID() && key &&
                                CFGetTypeID(key) == CFDataGetTypeID() && CFDataGetLength(key) == 16 &&
                                CFStringGetCString(addressString, address, sizeof(address), kCFStringEncodingUTF8);
        if (wellFormed && nameString && CFGetTypeID(nameString) == CFStringGetTypeID())
            CFStringGetCString(nameString, name, sizeof(name), kCFStringEncodingUTF8);
        switch (switch_joycon_dump_classify(&import, wellFormed, wellFormed && railHas(address))) {
        case SWITCH_JOYCON_DUMP_MALFORMED:
            os_log_error(logger, "joyconpaird: entry %ld of %{public}s is malformed", (long)i, kStagedPath);
            break;
        case SWITCH_JOYCON_DUMP_RAIL_NEWER:
            os_log(logger, "joyconpaird: %{public}s (%{public}s): the rail's pairing is newer than the dump's", name,
                   address);
            break;
        case SWITCH_JOYCON_DUMP_PAIR:
            switch_joycon_dump_paired(&import,
                                      pair(address, name, 0x000508, CFDataGetBytePtr(key), "Horizon, staged dump"));
            break;
        }
    }
    if (switch_joycon_dump_mark_imported(&import) && !writeFile(kImportedPath, staged))
        os_log_error(logger, "joyconpaird: cannot write %{public}s: %{errno}d", kImportedPath, errno);
    if (plist)
        CFRelease(plist);
    CFRelease(staged);
}

// Exits so that the starter tries again only when that can help: launchd restarts the agent after a failing exit
// (KeepAlive SuccessfulExit false), and only bluetoothd going away is cured by starting again. A dead session must not
// keep the lock either way.
static void quit(bool retry)
{
    exit(retry ? EXIT_FAILURE : EXIT_SUCCESS);
}

static void sessionEvent(BTSession session, int event, int result, void *context)
{
    if (event != 0 || result != 0) {
        os_log_error(logger, "joyconpaird: bluetoothd session event %d result %d", event, result);
        quit(result == 0 && (event == 1 || event == 2));  // detached or terminated: bluetoothd went away
    }
    int error = BTAccessoryManagerGetDefault(session, &manager);
    if (error) {
        os_log_error(logger, "joyconpaird: no accessory manager: %d", error);
        quit(false);
    }
    // Records already published, then any the rail driver publishes while this process lives.
    port = IONotificationPortCreate(kIOMainPortDefault);
    IONotificationPortSetDispatchQueue(port, queue);
    kern_return_t kr = IOServiceAddMatchingNotification(
        port, kIOFirstMatchNotification, IOServiceMatching("SwitchJoyConPairing"), matched, NULL, &records);
    if (kr != KERN_SUCCESS) {
        os_log_error(logger, "joyconpaird: IOServiceAddMatchingNotification: %#x", kr);
        quit(false);
    }
    matched(NULL, records);
    importStaged();
}

int main(void)
{
    logger = os_log_create("com.brunostjohn.switch.joyconpaird", "pairing");
    // One instance: iOS 17's BlueTool starts this at every chip boot, and the lock goes with the process. An agent whose
    // session just ended may still be on its way out: a few tries, 200 ms apart.
    const int lock = open(kLockPath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    bool locked = false;
    for (unsigned tries = 0; lock >= 0 && !locked && tries < 10; ++tries) {
        locked = !flock(lock, LOCK_EX | LOCK_NB);
        if (!locked)
            usleep(200000);
    }
    if (!locked)
        return 0;
    queue = dispatch_queue_create("com.brunostjohn.switch.joyconpaird", DISPATCH_QUEUE_SERIAL);
    void *mb = dlopen("/System/Library/PrivateFrameworks/MobileBluetooth.framework/MobileBluetooth", RTLD_NOW);
    BTSessionAttachWithQueue = mb ? dlsym(mb, "BTSessionAttachWithQueue") : NULL;
    BTAccessoryManagerGetDefault = mb ? dlsym(mb, "BTAccessoryManagerGetDefault") : NULL;
    BTDeviceAddressFromString = mb ? dlsym(mb, "BTDeviceAddressFromString") : NULL;
    BTAccessoryManagerSetLinkKeyEx = mb ? dlsym(mb, "BTAccessoryManagerSetLinkKeyEx") : NULL;
    if (!BTSessionAttachWithQueue || !BTAccessoryManagerGetDefault || !BTDeviceAddressFromString ||
        !BTAccessoryManagerSetLinkKeyEx) {
        os_log_error(logger, "joyconpaird: MobileBluetooth unavailable: %{public}s", dlerror());
        quit(false);
    }
    static const BTSessionCallbacks callbacks = {sessionEvent, {NULL, NULL, NULL}};
    dispatch_async(queue, ^{
        int error = BTSessionAttachWithQueue("com.brunostjohn.switch.joyconpaird", &callbacks, NULL, queue);
        if (error) {
            os_log_error(logger, "joyconpaird: BTSessionAttachWithQueue: %d", error);
            quit(false);
        }
    });
    dispatch_main();
}
