// SPDX-License-Identifier: BSD-2-Clause-Patent
// Wake sources with the panel off, the power journal and the wake stackshot (SwitchWake.h, SwitchWakeSources.h).
//
// Locks: the journal's ring and the per-source rate limits take one IOLock for a copy; the file writes run on their own
// thread calls, serialized by a second IOLock, so no caller (SwitchBacklight's and SwitchButtons' work loops,
// SwitchBluetooth's receive path) waits for the SD. The Bluetooth controllers' buttons (switch_bt_buttons) belong to
// SwitchBluetooth's receive path, which is serialized (its framing state has no lock either); a reset from a client
// change is a flag that path takes at its next packet.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <os/log.h>
#include <stdarg.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include "SwitchWake.h"
#include "../../kits/SwitchTime/SwitchTime.h"

// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" int vsnprintf(char *, size_t, const char *, va_list);
// SwitchBrightness.h: false from a display-off request until the panel is lit again.
extern "C" bool switch_display_is_on(void);
// osfmk/kern/kern_stackshot.c (IOPMrootDomain's sleep/wake failure stackshots use it the same way).
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" kern_return_t stack_snapshot_from_kernel(int pid, void *buf, uint32_t size, uint64_t flags,
                                                    uint64_t delta_since_timestamp, uint32_t pagetable_mask,
                                                    unsigned *bytes_traced);

namespace {
#if DEVELOPMENT || DEBUG
const char kJournal[] = "/private/var/Switch/power.log";
const char kPrevious[] = "/private/var/Switch/power.prev.log";
const char kCrashReporter[] = "/private/var/mobile/Library/Logs/CrashReporter";
#endif
constexpr uint64_t kSourceGapNS = 500000000ULL;
#if DEVELOPMENT || DEBUG
constexpr uint64_t kShotGapNS = 60000000000ULL;
constexpr uint32_t kShotBytes = 4u << 20, kShotMaxBytes = 8u << 20;  // XNU caps one at max_tracebuf_size (8 MiB here)
// osfmk/kern/debug.h: KCDATA_FORMAT, SAVE_LOADINFO (dyld images: user frames symbolicate), SAVE_KEXT_LOADINFO,
// THREAD_WAITINFO, NO_IO_STATS, INCLUDE_DRIVER_THREADS_IN_KERNEL (IOKit work loops); every task (pid -1).
constexpr uint64_t kShotFlags = 0x10000ULL | 0x02ULL | 0x08ULL | 0x1000000ULL | 0x800000ULL | 0x100000000ULL;
#endif

IOLock *volatile gLock;
uint64_t gLastSource[SWITCH_WAKE_SOURCES];  // gLock
#if DEVELOPMENT || DEBUG
IOLock *volatile gWriteLock;
thread_call_t volatile gFlushCall, gShotCall;
switch_power_ring gRing;                                     // gLock
uint64_t gLastShot;                                          // gLock
char gShotWhy[96];                                           // gLock
bool gCopiedPrevious, gWriteFailed;                          // gWriteLock
char gRender[SWITCH_POWER_LINES * SWITCH_POWER_LINE + 256];  // gWriteLock
#endif
switch_bt_buttons gButtons;  // SwitchBluetooth's receive path only (its ACL packets and link events)
#if DEVELOPMENT || DEBUG
bool gButtonsLogged;  // the same: the first controller report of the boot was logged
#endif
volatile bool gButtonsStale;  // a client came or went: every connection's buttons go (switch_wake_bt_reset)
uint64_t gReconnectUntil;     // switch_bt_reconnect_open's window after a Bluetooth wake (ns; atomic, 0 closed)

// NOLINTNEXTLINE(misc-const-correctness): the compare-and-swap below writes through slot
IOLock *once(IOLock *volatile *slot)
{
    IOLock *lock = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (lock)
        return lock;
    IOLock *fresh = IOLockAlloc();
    IOLock *expected = nullptr;
    if (!fresh)
        return nullptr;
    if (__atomic_compare_exchange_n(slot, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    IOLockFree(fresh);
    return expected;
}

#if DEVELOPMENT || DEBUG
// NOLINTNEXTLINE(misc-const-correctness): the compare-and-swap below writes through slot
thread_call_t onceCall(thread_call_t volatile *slot, thread_call_func_t func)
{
    thread_call_t call = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (call)
        return call;
    thread_call_t fresh = thread_call_allocate_with_options(func, nullptr, THREAD_CALL_PRIORITY_KERNEL, 0);
    thread_call_t expected = nullptr;
    if (!fresh)
        return nullptr;
    if (__atomic_compare_exchange_n(slot, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    thread_call_free(fresh);
    return expected;
}
#endif

#if DEVELOPMENT || DEBUG
// "YYYY-MM-DD-HHMMSS" (UTC), as the panic records are named (days to civil date: H. Hinnant's algorithm).
void utcStamp(char *out, size_t size)
{
    clock_sec_t secs = 0;
    clock_usec_t usecs = 0;
    clock_get_calendar_microtime(&secs, &usecs);
    const uint64_t t = static_cast<uint64_t>(secs);
    const auto second = static_cast<unsigned>(t % 60);
    const auto minute = static_cast<unsigned>(t / 60 % 60);
    const auto hour = static_cast<unsigned>(t / 3600 % 24);
    const int64_t days = static_cast<int64_t>(t / 86400) + 719468;
    const int64_t era = days / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;
    const long long year = static_cast<long long>(yoe) + era * 400 + static_cast<long long>(month <= 2);
    snprintf(out, size, "%04lld-%02u-%02u-%02u%02u%02u", year, month, day, hour, minute, second);
}

// The whole file: created, or overwritten and cut to `length` (vnode_open does not truncate), owned by uid/gid when
// uid >= 0, fsynced. durable: F_FULLFSYNC as well, which commits APFS's transaction (an fsync may leave the new
// extents in an uncommitted one, which a power cut, or the SD going down for system sleep, then loses). 0 or an errno.
int writeFile(const char *path, const void *bytes, size_t length, int mode, int uid, bool durable = false)
{
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    int error = vnode_open(path, O_CREAT | FWRITE | O_NOFOLLOW, mode, 0, &vp, ctx);
    if (error)
        return error;
    int resid = 0;
    error =
        vn_rdwr(UIO_WRITE, vp, reinterpret_cast<caddr_t>(reinterpret_cast<uintptr_t>(bytes)), static_cast<int>(length),
                0, UIO_SYSSPACE, IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    if (!error && resid)
        error = EIO;
    if (!error) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_SET(&va, va_data_size, (uint64_t)length);
        if (uid >= 0) {
            VATTR_SET(&va, va_uid, (uid_t)uid);
            VATTR_SET(&va, va_gid, (gid_t)uid);
        }
        error = vnode_setattr(vp, &va, ctx);
    }
    if (!error)
        error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
    if (!error && durable)
        error = VNOP_IOCTL(vp, F_FULLFSYNC, nullptr, 0, ctx);
    const int closeError = vnode_close(vp, FWRITE, ctx);
    return error ? error : closeError;
}

// The last boot's power.log to power.prev.log, once per boot, before this boot's first write replaces it.
void copyPrevious()
{
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    if (vnode_open(kJournal, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx))
        return;  // none yet
    int resid = 0;
    const int error = vn_rdwr(UIO_READ, vp, gRender, static_cast<int>(sizeof(gRender)), 0, UIO_SYSSPACE, IO_NOAUTH,
                              vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    vnode_close(vp, FREAD, ctx);
    if (!error && sizeof(gRender) > static_cast<size_t>(resid))
        (void)writeFile(kPrevious, gRender, sizeof(gRender) - static_cast<size_t>(resid), 0644, -1);
}

void writeJournal(bool durable)
{
    IOLock *lock = once(&gLock);
    IOLock *writeLock = once(&gWriteLock);
    if (!lock || !writeLock)
        return;
    IOLockLock(writeLock);
    if (!gCopiedPrevious) {
        gCopiedPrevious = true;
        copyPrevious();
    }
    char stamp[24];
    utcStamp(stamp, sizeof(stamp));
    int used = snprintf(gRender, sizeof(gRender), "SwitchPower journal, written %s UTC at %llu ms uptime\n", stamp,
                        switch_now_ns() / 1000000);
    if (used < 0)
        used = 0;
    IOLockLock(lock);
    const size_t length = static_cast<size_t>(used) +
                          switch_power_ring_render(&gRing, gRender + used, sizeof(gRender) - static_cast<size_t>(used));
    IOLockUnlock(lock);
    const int error = writeFile(kJournal, gRender, length, 0644, -1, durable);
    if (error && !gWriteFailed)
        IOLog("SwitchPower: %s not written (%d)\n", kJournal, error);
    gWriteFailed = error != 0;
    IOLockUnlock(writeLock);
}

void flushCall(thread_call_param_t, thread_call_param_t)
{
    writeJournal(false);
}

void shotCall(thread_call_param_t, thread_call_param_t)
{
    IOLock *lock = once(&gLock);
    char why[sizeof(gShotWhy)] = "";
    if (lock) {
        IOLockLock(lock);
        strlcpy(why, gShotWhy, sizeof(why));
        IOLockUnlock(lock);
    }
    uint32_t size = kShotBytes;
    void *buffer = nullptr;
    unsigned bytes = 0;
    kern_return_t kr = KERN_FAILURE;
    for (;;) {
        buffer = IOMallocData(size);
        if (!buffer)
            break;
        bytes = 0;
        kr = stack_snapshot_from_kernel(-1, buffer, size, kShotFlags, 0, 0, &bytes);
        if (kr != KERN_INSUFFICIENT_BUFFER_SIZE || size >= kShotMaxBytes)
            break;
        IOFreeData(buffer, size);
        buffer = nullptr;
        size *= 2;
    }
    if (!buffer || kr != KERN_SUCCESS || !bytes) {
        switch_power_journal("wake stackshot (%s) failed: %#x, %u bytes of %u", why, kr, bytes, size);
        if (buffer)
            IOFreeData(buffer, size);
        switch_power_journal_flush(false);
        return;
    }
    char stamp[24];
    char path[160];
    utcStamp(stamp, sizeof(stamp));
    snprintf(path, sizeof(path), "/private/var/Switch/wake-%s.kcdata", stamp);
    const int error = writeFile(path, buffer, bytes, 0644, -1);
    snprintf(path, sizeof(path), "%s/Switch-wake-%s.kcdata", kCrashReporter, stamp);
    const int copyError = error ? error : writeFile(path, buffer, bytes, 0644, 501);
    IOFreeData(buffer, size);
    switch_power_journal("wake stackshot (%s): wake-%s.kcdata, %u bytes (written: %d, CrashReporter copy: %d)", why,
                         stamp, bytes, error, copyError);
    switch_power_journal_flush(false);
}
#endif
}  // namespace

#if DEVELOPMENT || DEBUG
extern "C" void switch_power_journal(const char *format, ...)
{
    char text[SWITCH_POWER_LINE - 16];
    char line[SWITCH_POWER_LINE];
    va_list args;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    const uint64_t ms = switch_now_ns() / 1000000;
    snprintf(line, sizeof(line), "%llu.%03llu %s", ms / 1000, ms % 1000, text);
    os_log(OS_LOG_DEFAULT, "SwitchPower: %s", line);
    IOLock *lock = once(&gLock);
    if (!lock)
        return;
    IOLockLock(lock);
    switch_power_ring_append(&gRing, line);
    IOLockUnlock(lock);
}

extern "C" void switch_power_journal_flush(bool wait)
{
    if (wait) {
        writeJournal(true);
        return;
    }
    if (thread_call_t call = onceCall(&gFlushCall, flushCall))
        thread_call_enter(call);
}
#endif

extern "C" void switch_wake_event(unsigned source)
{
    if (source >= SWITCH_WAKE_SOURCES || switch_display_is_on())
        return;
    IOLock *lock = once(&gLock);
    if (!lock)
        return;
    const uint64_t now = mach_absolute_time();
    IOLockLock(lock);
    const bool fresh = (gLastSource[source] == 0u) || switch_abs_to_ns(now - gLastSource[source]) >= kSourceGapNS;
    if (fresh)
        gLastSource[source] = now;
    IOLockUnlock(lock);
    if (!fresh)
        return;
    switch_power_journal("wake source: %s with the panel off%s", switch_wake_source_name(source),
                         source == SWITCH_WAKE_BT_HOME ? " (console Home clicked for it)" : "");
    switch_backlight_expect_wake(source);
}

extern "C" void switch_wake_bt_acl(const uint8_t *acl, size_t length)
{
    uint16_t handle = 0;
    uint8_t report = 0;
    const int held = switch_wake_bt_buttons(acl, length, &handle, &report);
    if (held < 0)
        return;
    if (__atomic_exchange_n(&gButtonsStale, false, __ATOMIC_ACQ_REL))
        bzero(&gButtons, sizeof(gButtons));
    const bool dark = !switch_display_is_on();
    const switch_bt_buttons_step step =
        switch_bt_buttons_report(&gButtons, handle, (uint8_t)held, switch_now_ns(), dark);
#if DEVELOPMENT || DEBUG
    const bool first = !gButtonsLogged;
    gButtonsLogged = true;
    if (first) {
        os_log(OS_LOG_DEFAULT,
               "SwitchWake: first Nintendo controller report of the boot: handle %#x, report %#04x, "
               "Home/Capture %#x; their clicks go to the console buttons",
               handle, report, held);
    }
#endif
    // A connection's first report with the panel off (a controller that reconnected to wake the console): what it held.
    if (step.fresh && dark) {
        switch_power_journal("Bluetooth controller %#x connected with the panel off: report %#04x, Home/Capture %#x",
                             handle, report, held);
    }
    uint8_t click = step.click;
    uint64_t until = __atomic_load_n(&gReconnectUntil, __ATOMIC_ACQUIRE);
    bool reconnect = false;
    if (until) {
        reconnect = switch_bt_reconnect_take(&until, switch_now_ns(), step.fresh, dark);
        if (!until)
            __atomic_store_n(&gReconnectUntil, 0, __ATOMIC_RELEASE);
    }
    if (reconnect && !(click & SWITCH_BT_HOME)) {
        switch_power_journal("Bluetooth controller %#x reconnected after a Bluetooth wake: console Home", handle);
        click |= SWITCH_BT_HOME;
    }
    if ((step.pressed & SWITCH_BT_HOME) || reconnect)
        switch_wake_event(SWITCH_WAKE_BT_HOME);  // nothing when lit
    if (click)
        switch_buttons_bt_click(click);
}

extern "C" unsigned switch_wake_bt_controllers(uint16_t *handles, unsigned max)
{
    if (__atomic_load_n(&gButtonsStale, __ATOMIC_ACQUIRE))
        return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < SWITCH_BT_BUTTON_LINKS && n < max; ++i) {
        if (gButtons.used[i])
            handles[n++] = gButtons.handle[i];
    }
    return n;
}

extern "C" void switch_wake_bt_slept(void)
{
    uint64_t until = 0;
    switch_bt_reconnect_open(&until, switch_now_ns());
    __atomic_store_n(&gReconnectUntil, until, __ATOMIC_RELEASE);
}

extern "C" void switch_wake_bt_gone(uint16_t handle)
{
    switch_bt_buttons_gone(&gButtons, handle);
}

extern "C" void switch_wake_bt_reset(void)
{
    __atomic_store_n(&gButtonsStale, true, __ATOMIC_RELEASE);
}

#if DEVELOPMENT || DEBUG
extern "C" void switch_wake_stackshot(const char *why)
{
    IOLock *lock = once(&gLock);
    thread_call_t call = onceCall(&gShotCall, shotCall);
    if (!lock || !call)
        return;
    const uint64_t now = mach_absolute_time();
    IOLockLock(lock);
    const bool fresh = (gLastShot == 0u) || switch_abs_to_ns(now - gLastShot) >= kShotGapNS;
    if (fresh) {
        gLastShot = now;
        strlcpy(gShotWhy, why ? why : "", sizeof(gShotWhy));
    }
    IOLockUnlock(lock);
    if (fresh)
        thread_call_enter(call);
}
#endif
