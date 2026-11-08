// SPDX-License-Identifier: BSD-2-Clause-Patent
// Original HID observation for the owned offline input source (NativeHidInput), which this port never starts. No
// entitlement or original provider is replaced.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSKext.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" {

volatile uint32_t native_hid_stage, native_hid_resource_found;
volatile int native_hid_load_status = -1;
volatile int native_hid_matching_load_status = -1;
volatile uint32_t native_hid_class_sizes[5];
volatile int native_hid_input_meta_status = -1;
volatile uint64_t native_hid_input_object;
volatile uint32_t native_hid_input_events[8];
volatile int native_hid_input_results[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
volatile uint32_t native_hid_input_command;
char native_hid_input_registry[2048];
char native_hid_input_props[1024];
extern volatile uint32_t native_legacy_transfer_calls;

bool native_hid_input_requested(void)
{
    if (native_legacy_transfer_calls < 2)
        return false;
    if (native_hid_input_command != 1)
        return false;
    native_hid_input_command = 0;
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): bounded by depth
static void describeInput(const IORegistryEntry *entry, unsigned depth, size_t &used)
{
    if (depth > 4 || used >= sizeof(native_hid_input_registry) - 128)
        return;
    const int count = snprintf(native_hid_input_registry + used, sizeof(native_hid_input_registry) - used,
                               "%u %s [%s]\n", depth, entry->getName(), entry->getMetaClass()->getClassName());
    if (count <= 0 || (size_t)count >= sizeof(native_hid_input_registry) - used)
        return;
    used += count;
    OSIterator *it = entry->getChildIterator(gIOServicePlane);
    if (!it)
        return;
    while (const OSObject *child = it->getNextObject()) {
        const IORegistryEntry *next = OSDynamicCast(IORegistryEntry, child);
        if (next)
            describeInput(next, depth + 1, used);
    }
    it->release();
}

// Compact one property of the first IOHIDEventDriver below entry: XML tags dropped, key end -> '=', value end -> ' '.
// NOLINTNEXTLINE(misc-no-recursion): bounded by depth
static LIBKERN_RETURNS_NOT_RETAINED IORegistryEntry *findEventDriver(const IORegistryEntry *parent, unsigned depth)
{
    if (!depth)
        return nullptr;
    OSIterator *it = parent->getChildIterator(gIOServicePlane);
    if (!it)
        return nullptr;
    IORegistryEntry *found = nullptr;
    while (const OSObject *child = it->getNextObject()) {
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, child);
        if (!entry)
            continue;
        if (entry->metaCast("IOHIDEventDriver")) {
            found = entry;
            break;
        }
        found = findEventDriver(entry, depth - 1);
        if (found)
            break;
    }
    it->release();
    return found;
}

static void appendProperty(const IORegistryEntry *driver, const char *key, const char *label, size_t &used)
{
    const OSObject *value = driver->copyProperty(key);
    const int n = snprintf(native_hid_input_props + used, sizeof(native_hid_input_props) - used, "%s:", label);
    if (n > 0)
        used += n;
    if (value) {
        OSSerialize *text = OSSerialize::withCapacity(4096);
        if (text && value->serialize(text)) {
            const char *in = text->text();
            bool tag = false;
            char name[16];
            unsigned nameLen = 0;
            for (unsigned i = 0; in && in[i] && used < sizeof(native_hid_input_props) - 2; ++i) {
                const char c = in[i];
                if (c == '<') {
                    tag = true;
                    nameLen = 0;
                    continue;
                }
                if (tag) {
                    if (c == '>') {
                        tag = false;
                        name[nameLen] = 0;
                        if (!strcmp(name, "/key")) {
                            native_hid_input_props[used++] = '=';
                        } else if (name[0] == '/') {
                            native_hid_input_props[used++] = ' ';
                        } else if (!strcmp(name, "true/") || !strcmp(name, "false/")) {
                            used += snprintf(native_hid_input_props + used, sizeof(native_hid_input_props) - used,
                                             "%s ", name[0] == 't' ? "T" : "F");
                        }
                    } else if (nameLen < sizeof(name) - 1) {
                        name[nameLen++] = c;
                    }
                    continue;
                }
                if (c == '\n' || c == '\t')
                    continue;
                native_hid_input_props[used++] = c;
            }
        }
        OSSafeReleaseNULL(text);
        OSSafeReleaseNULL(value);
    } else if (used < sizeof(native_hid_input_props) - 6) {
        memcpy(native_hid_input_props + used, "none ", 5);
        used += 5;
    }
    native_hid_input_props[used] = 0;
}

__attribute__((noinline)) void native_hid_live_checkpoint(const IOService *device)
{
    size_t used = 0;
    native_hid_input_registry[0] = 0;
    describeInput(device, 0, used);
    if (IORegistryEntry *driver = findEventDriver(device, 4)) {
        size_t props = 0;
        native_hid_input_props[0] = 0;
        // backboardd's digitizer service classification reads Built-In/DisplayIntegrated from the event service it
        // opened, not from the HID device below it; publish them on the service itself (idempotent, once per second).
        // Set once only: every setProperty on a service with attached user clients is a registry property-change
        // notification, and repeating it each second keeps backboardd/hidd busy re-evaluating the service.
        static IORegistryEntry *publishedFor;
        if (device->getProperty("Built-In") && publishedFor != driver) {
            publishedFor = driver;
            driver->setProperty("Built-In", kOSBooleanTrue);
            driver->setProperty("DisplayIntegrated", kOSBooleanTrue);
            // backboardd (-[BKHIDDirectTouchEventProcessor processEvent:sender:dispatcher:], decoded from the target) drops a
            // digitizer event unless the sending service has PrimaryUsagePage 0x0D and PrimaryUsage 1, 4 or 0x20
            // ("ignoring digitizer event ... from unsupported service"). The combined device lists its keyboard collection
            // first, so the services would publish (1,6); publish Digitizer/Touch Screen on the services after they have
            // matched (setting it on the device would change driver matching, which is observed to depend on the layout).
            driver->setProperty("PrimaryUsagePage", (unsigned long long)0x0d, 32);
            driver->setProperty("PrimaryUsage", (unsigned long long)4, 32);
            // backboardd's digitizer filter is {DeviceUsagePage, DeviceUsage, Built-In}; make the flag visible on the
            // interface (the event service's provider) as well, not only on the device two levels down.
            if (IORegistryEntry *interface = driver->getParentEntry(gIOServicePlane)) {
                interface->setProperty("Built-In", kOSBooleanTrue);
                interface->setProperty("DisplayIntegrated", kOSBooleanTrue);
                interface->setProperty("PrimaryUsagePage", (unsigned long long)0x0d, 32);
                interface->setProperty("PrimaryUsage", (unsigned long long)4, 32);
            }
        }
        // The Digitizer/DebugState dumps (appendProperty) are not read on hardware runs without a photograph and walk
        // driver-internal state from a foreign thread; only the cheap Built-In readback stays.
        appendProperty(driver, "Built-In", "BI", props);
        appendProperty(driver, "PrimaryUsagePage", "PP", props);
        appendProperty(driver, "PrimaryUsage", "PU", props);
    }
    __asm__ volatile("" ::: "memory");
}

void native_hid_input_observed(unsigned event, int result)
{
    if (event < 8) {
        native_hid_input_events[event] = native_hid_input_events[event] + 1;
        native_hid_input_results[event] = result;
    }
}

__attribute__((noinline)) void native_hid_checkpoint(void)
{
    __asm__ volatile("" ::: "memory");
}
}
