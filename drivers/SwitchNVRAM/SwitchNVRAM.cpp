// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVRAM controller for XNU's IODTNVRAM, kept in a file on the Data volume: /private/var/Switch/nvram.bin (format in
// SwitchNVRAMStore.h).
//
// IODTNVRAM builds its variables from /chosen/nvram-proxy-data (an empty bank from the packaging) when the platform
// expert starts, and commits them through its controller: select, eraseBank and write of the whole bank, then sync().
// This controller holds the bank in RAM, seeded from the same proxy. Without a controller every sync fails with
// kIOReturnNotReady and the "IONVRAM" resource is never published (IOPlatformExpert::registerNVRAMController publishes
// it); the original AppleImage4 waits for that resource, without a timeout, before every NVRAM write.
//
// Restore. Nothing can read the Data volume before the kernel mounts it, just before launchd starts (kern_exec.c
// load_init_program), so until then IODTNVRAM has only the proxy and what the kernel set since boot (AppleImage4, the
// boot-args default). switch_nvram_data_mounted(), called there, reads the file on a kernel thread and gives its
// newest valid bank to IODTNVRAM (IONVRAMCHRPHandler::switchRestore, XNU patch), which adds each variable not set
// since boot; then Lockdown Mode, which the kernel read before, is read again. Userland sees the saved NVRAM from its
// first read. A missing or unusable file means an empty NVRAM and one log line.
//
// Writes. sync() runs under IODTNVRAM's locks: it copies the bank and, when it becomes dirty, arms a 2 s timer;
// it never waits for I/O. The timer writes the bank to the slot that does not hold the current one and fsyncs it, so
// a burst of syncs is one write, and a bank whose variables did not change (only IODTNVRAM's generation) is not
// written at all. switch_nvram_flush() writes at once and waits: this driver calls it on system sleep
// (kIOMessageSystemWillSleep) and on shutdown or restart (kIOMessageSystemPagingOff, the last message before the file
// systems are unmounted; IOPMrootDomain's PESyncNVRAM comes after the unmount). Each flush first has IODTNVRAM commit
// the variables set without a sync, as that PESyncNVRAM does on a board with flash.
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IONVRAM.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/nvram/IONVRAMController.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSData.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include "../../kits/SwitchDT/SwitchDTProperty.h"
#include "SwitchNVRAM.h"
#include "SwitchNVRAMStore.h"

// XNU patch (iokit/Kernel/IONVRAMCHRPHandler.cpp): adds the variables of a saved CHRP bank to IODTNVRAM, except those
// set since boot and those named in skip; counts the ones added and the ones that did not fit.
extern "C" IOReturn switch_nvram_restore(IODTNVRAM *nvram, const uint8_t *image, size_t length, const char *const *skip,
                                         uint32_t *added, uint32_t *refused);
// XNU patch (bsd/kern/lockdown_mode.c): reads Lockdown Mode's ldm again, which lockdown_mode_init read before Data was
// mounted. ldm is kernel-only: the kernel task's read.
extern "C" void lockdown_mode_nvram_restored(void);

namespace {
const char kPath[] = "/private/var/Switch/nvram.bin";
constexpr uint32_t kWriteDelayMs = 2000;
// The apple_nvram_header (CHRP header, adler32, generation): what changes on every IODTNVRAM sync even when no
// variable did. The variables follow it.
constexpr uint32_t kBankHeader = 32;
// Variables iBoot acts on for one boot and then deletes. No iBoot deletes them here, so restored they would apply to
// every later boot (darkboot: the next boot with the display dark).
const char *const kNotRestored[] = {"darkboot", nullptr};

uint32_t chosenU32(const IORegistryEntry *chosen, const char *key, uint32_t fallback)
{
    uint32_t value = 0;
    return switch_dt_value(chosen, key, &value) ? value : fallback;
}
}  // namespace

class SwitchNVRAM;
static SwitchNVRAM *gSwitchNVRAM;

class SwitchNVRAM : public IONVRAMController {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchNVRAM) uint32_t bankSize = 0, bankCount = 0, current = 0, stride = 0;
    // banks: what IODTNVRAM wrote (its controller lock). staged: the last bank sync() committed; saved: the bank the
    // file's current slot holds; pending, slot: the writer's copy and its slot.
    uint8_t *banks = nullptr, *staged = nullptr, *saved = nullptr, *pending = nullptr, *slot = nullptr;
    IOLock *lock = nullptr;       // staged and the flags below; never held across I/O
    IOLock *writeLock = nullptr;  // one writer at a time, across the file I/O; taken before lock
    thread_call_t writeCall = nullptr;
    IODTNVRAM *nvram = nullptr;
    IONotifier *power = nullptr;
    uint64_t sequence = 0;     // the current slot's (writeLock)
    unsigned currentSlot = 1;  // so that the first write goes to slot 0 (writeLock)
    bool dirty = false;        // staged is newer than the file
    bool armed = false;        // writeCall is pending
    bool mounted = false;      // Data is mounted: writes can start
    bool stopped = false;      // shutting down: syncs stay in RAM
    bool writeFailed = false;  // a failed write is logged once until one succeeds (writeLock)

    bool loadChosenImage();
    void armLocked();
    int writeOut();
    int writeSlot(unsigned index);
    static void writeTimer(thread_call_param_t self, thread_call_param_t);
    static void restoreThread(void *parameter, wait_result_t);
    static IOReturn powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t);

public:
    bool start(IOService *provider) override;
    void free() override;
    void sync() override;
    IOReturn select(uint32_t bank) override;
    IOReturn eraseBank() override;
    IOReturn read(IOByteCount offset, UInt8 *buffer, IOByteCount length) override;
    IOReturn write(IOByteCount offset, UInt8 *buffer, IOByteCount length) override;
    uint32_t dataMounted();
    int flush(bool final);
    static uint32_t restoreFromData();
};

OSDefineMetaClassAndStructors(SwitchNVRAM, IONVRAMController)

    // Same geometry IODTNVRAM read from /chosen; the current bank starts as the proxy image, so the handler's
    // findCurrentBank() sees its own generation.
    bool SwitchNVRAM::loadChosenImage()
{
    const IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen)
        return false;
    bankSize = chosenU32(chosen, "nvram-bank-size", 0);
    bankCount = chosenU32(chosen, "nvram-bank-count", 1);
    current = chosenU32(chosen, "nvram-current-bank", 0);
    bool ok = bankSize > kBankHeader && bankSize <= SWITCH_NVRAM_STORE_MAX_IMAGE && (bankCount != 0u) &&
              bankCount <= 8 && current < bankCount;
    if (ok)
        banks = IONewZeroData(uint8_t, static_cast<vm_size_t>(bankSize) * bankCount);
    ok = ok && (banks != nullptr);
    if (ok)
        switch_dt_read(chosen, "nvram-proxy-data", banks + static_cast<size_t>(current) * bankSize, bankSize);
    chosen->release();
    return ok;
}

bool SwitchNVRAM::start(IOService *provider)
{
    if (!IONVRAMController::start(provider))
        return false;
    if (!loadChosenImage()) {
        IOLog("SwitchNVRAM: /chosen has no usable nvram-bank-size/count\n");
        return false;
    }
    stride = switch_nvram_store_stride(bankSize);
    staged = IONewZeroData(uint8_t, bankSize);
    saved = IONewZeroData(uint8_t, bankSize);
    pending = IONewZeroData(uint8_t, bankSize);
    slot = IONewZeroData(uint8_t, stride);
    lock = IOLockAlloc();
    writeLock = IOLockAlloc();
    writeCall = thread_call_allocate_with_options(writeTimer, this, THREAD_CALL_PRIORITY_LOW, 0);
    if (!staged || !saved || !pending || !slot || !lock || !writeLock || !writeCall) {
        IOLog("SwitchNVRAM: out of memory\n");
        return false;
    }
    return true;
}

void SwitchNVRAM::free()
{
    if (writeCall)
        thread_call_free(writeCall);
    if (lock)
        IOLockFree(lock);
    if (writeLock)
        IOLockFree(writeLock);
    if (banks)
        IODeleteData(banks, uint8_t, bankSize *bankCount);
    if (staged)
        IODeleteData(staged, uint8_t, bankSize);
    if (saved)
        IODeleteData(saved, uint8_t, bankSize);
    if (pending)
        IODeleteData(pending, uint8_t, bankSize);
    if (slot)
        IODeleteData(slot, uint8_t, stride);
    IONVRAMController::free();
}

// IODTNVRAM calls select/erase/read/write/sync with its controller lock held.
IOReturn SwitchNVRAM::select(uint32_t bank)
{
    if (bank >= bankCount)
        return kIOReturnBadArgument;
    current = bank;
    return kIOReturnSuccess;
}

IOReturn SwitchNVRAM::eraseBank()
{
    bzero(banks + static_cast<size_t>(current) * bankSize, bankSize);
    return kIOReturnSuccess;
}

IOReturn SwitchNVRAM::read(IOByteCount offset, UInt8 *buffer, IOByteCount length)
{
    if (!buffer || offset > bankSize || length > bankSize - offset)
        return kIOReturnBadArgument;
    memcpy(buffer, banks + static_cast<size_t>(current) * bankSize + offset, length);
    return kIOReturnSuccess;
}

IOReturn SwitchNVRAM::write(IOByteCount offset, UInt8 *buffer, IOByteCount length)
{
    if (!buffer || offset > bankSize || length > bankSize - offset)
        return kIOReturnBadArgument;
    memcpy(banks + static_cast<size_t>(current) * bankSize + offset, buffer, length);
    return kIOReturnSuccess;
}

// After each full-bank write. Before Data is mounted the bank only stays here (dataMounted() then has IODTNVRAM
// commit again, with the saved variables added).
void SwitchNVRAM::sync()
{
    IOLockLock(lock);
    memcpy(staged, banks + static_cast<size_t>(current) * bankSize, bankSize);
    dirty = true;
    armLocked();
    IOLockUnlock(lock);
}

void SwitchNVRAM::armLocked()
{
    if (!mounted || stopped || armed)
        return;
    armed = true;
    uint64_t deadline = 0;
    clock_interval_to_deadline(kWriteDelayMs, kMillisecondScale, &deadline);
    thread_call_enter_delayed(writeCall, deadline);
}

void SwitchNVRAM::writeTimer(thread_call_param_t self, thread_call_param_t)
{
    SwitchNVRAM *controller = static_cast<SwitchNVRAM *>(self);
    IOLockLock(controller->lock);
    controller->armed = false;
    IOLockUnlock(controller->lock);
    (void)controller->writeOut();
}

// The staged bank to the slot that does not hold the current one, then fsync. A failure keeps it staged for the next
// sync or flush (no retry timer).
int SwitchNVRAM::writeOut()
{
    IOLockLock(writeLock);
    IOLockLock(lock);
    const bool write = dirty && mounted;
    if (write) {
        memcpy(pending, staged, bankSize);
        dirty = false;
    }
    IOLockUnlock(lock);
    int error = 0;
    if (write && memcmp(pending + kBankHeader, saved + kBankHeader, bankSize - kBankHeader)) {
        switch_nvram_store_fill(slot, pending, bankSize, sequence + 1);
        error = writeSlot(currentSlot ^ 1);
        if (!error) {
            sequence++;
            currentSlot ^= 1;
            memcpy(saved, pending, bankSize);
            writeFailed = false;
        } else {
            IOLockLock(lock);
            dirty = true;  // staged is this bank or a newer one
            IOLockUnlock(lock);
            if (!writeFailed)
                IOLog("SwitchNVRAM: %s not written (%d); kept in RAM for the next sync\n", kPath, error);
            writeFailed = true;
        }
    }
    IOLockUnlock(writeLock);
    return error;
}

int SwitchNVRAM::writeSlot(unsigned index)
{
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    int error = vnode_open(kPath, O_CREAT | FWRITE | O_NOFOLLOW, 0600, 0, &vp, ctx);
    if (error)
        return error;
    int resid = 0;
    error = vn_rdwr(UIO_WRITE, vp, reinterpret_cast<caddr_t>(slot), static_cast<int>(stride),
                    static_cast<off_t>(index) * stride, UIO_SYSSPACE, IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx),
                    &resid, vfs_context_proc(ctx));
    if (!error && resid)
        error = EIO;
    if (!error)
        error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
    const int closeError = vnode_close(vp, FWRITE, ctx);
    return error ? error : closeError;
}

// Once, with Data mounted and before launchd (restoreThread). Returns how many variables were restored.
uint32_t SwitchNVRAM::dataMounted()
{
    OSDictionary *match = IOService::serviceMatching("IODTNVRAM");
    const IOService *service = match ? IOService::copyMatchingService(match) : nullptr;
    OSSafeReleaseNULL(match);
    nvram = OSDynamicCast(IODTNVRAM, service);
    if (!nvram) {
        OSSafeReleaseNULL(service);
        IOLog("SwitchNVRAM: no IODTNVRAM; NVRAM not restored or saved\n");
        return 0;
    }
    const size_t capacity = 2 * static_cast<size_t>(stride);
    uint8_t *file = IONewZeroData(uint8_t, capacity);
    size_t length = 0;
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    int error = file ? vnode_open(kPath, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx) : ENOMEM;
    if (!error) {
        int resid = 0;
        error = vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(file), static_cast<int>(capacity), 0, UIO_SYSSPACE,
                        IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        length = capacity - static_cast<size_t>(resid);
        vnode_close(vp, FREAD, ctx);
    }
    uint32_t added = 0;
    uint32_t refused = 0;
    uint64_t found = 0;
    const int picked = error ? -1 : switch_nvram_store_pick(file, length, bankSize, &found);
    if (error == ENOENT) {
        IOLog("SwitchNVRAM: no %s yet; NVRAM starts empty\n", kPath);
    } else if (error) {
        IOLog("SwitchNVRAM: %s unreadable (%d); NVRAM starts empty\n", kPath, error);
    } else if (picked < 0) {
        IOLog("SwitchNVRAM: %s (%u bytes) has no valid slot; NVRAM starts empty\n", kPath,
              static_cast<unsigned>(length));
    } else {
        const uint8_t *image =
            file + static_cast<size_t>(static_cast<unsigned>(picked)) * stride + SWITCH_NVRAM_STORE_HEADER;
        const IOReturn result = switch_nvram_restore(nvram, image, bankSize, kNotRestored, &added, &refused);
        if (result == kIOReturnSuccess) {
            // The writer's state (no writer runs before mounted is set).
            memcpy(saved, image, bankSize);
            sequence = found;
            currentSlot = static_cast<unsigned>(picked);
            IOLog("SwitchNVRAM: %u variables restored from %s (write %llu, slot %d)%s\n", added, kPath, found, picked,
                  refused ? "; some did not fit the common partition" : "");
        } else {
            IOLog("SwitchNVRAM: IODTNVRAM refused the bank in %s (%#x); NVRAM starts empty\n", kPath, result);
        }
    }
    if (file)
        IODeleteData(file, uint8_t, capacity);
    // A bank IODTNVRAM committed before now lacks the restored variables: drop it and have the merged set committed.
    IOLockLock(lock);
    mounted = true;
    const bool recommit = dirty && (added != 0u);
    if (recommit) {
        dirty = false;
    } else if (dirty) {
        armLocked();
    }
    IOLockUnlock(lock);
    power = registerPrioritySleepWakeInterest(powerMessage, this);
    if (!power)
        IOLog("SwitchNVRAM: no sleep/shutdown notifications; NVRAM is saved on syncs only\n");
    if (recommit)
        nvram->sync();
    return added;
}

namespace {
struct Restore {
    SwitchNVRAM *controller;
    uint32_t added;
    bool done;
};
}  // namespace

// On a kernel thread, waited for: IODTNVRAM lets only the kernel task read and write kernel-only variables (ldm,
// nonce-seeds), and load_init_program runs in launchd's process.
void SwitchNVRAM::restoreThread(void *parameter, wait_result_t)
{
    Restore *restore = static_cast<Restore *>(parameter);
    SwitchNVRAM *controller = restore->controller;
    const uint32_t added = controller->dataMounted();
    if (added)
        lockdown_mode_nvram_restored();
    IOLockLock(controller->lock);
    restore->added = added;
    restore->done = true;
    IOLockWakeup(controller->lock, restore, true);
    IOLockUnlock(controller->lock);
    thread_terminate(current_thread());
}

uint32_t SwitchNVRAM::restoreFromData()
{
    if (!gSwitchNVRAM)
        return 0;
    Restore restore = {gSwitchNVRAM, 0, false};
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(restoreThread, &restore, &thread) != KERN_SUCCESS) {
        IOLog("SwitchNVRAM: no restore thread; NVRAM not restored or saved\n");
        return 0;
    }
    thread_deallocate(thread);
    IOLockLock(gSwitchNVRAM->lock);
    while (!restore.done)
        IOLockSleep(gSwitchNVRAM->lock, &restore, THREAD_UNINT);
    IOLockUnlock(gSwitchNVRAM->lock);
    return restore.added;
}

// Now, waiting for the write: has IODTNVRAM commit what is set but not synced, then writes the bank. final (shutdown)
// keeps every later sync in RAM, since the file systems are about to be unmounted.
int SwitchNVRAM::flush(bool final)
{
    IOLockLock(lock);
    const bool ready = mounted && !stopped;
    stopped = stopped || final;
    IOLockUnlock(lock);
    if (!ready)
        return 0;
    if (thread_call_cancel(writeCall)) {
        IOLockLock(lock);
        armed = false;
        IOLockUnlock(lock);
    }
    nvram->sync();
    return writeOut();
}

IOReturn SwitchNVRAM::powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t)
{
    if (type == kIOMessageSystemWillSleep || type == kIOMessageSystemPagingOff)
        (void)static_cast<SwitchNVRAM *>(target)->flush(type == kIOMessageSystemPagingOff);
    return kIOReturnSuccess;
}

extern "C" int switch_nvram_publish(void)
{
    static int status = -1;  // boot-only: -1 untouched, 0 published, 1 failed
    if (status >= 0)
        return status;
    status = 1;
    IOService *resources = IOService::getResourceService();
    SwitchNVRAM *nvram = OSTypeAlloc(SwitchNVRAM);
    if (!resources || !nvram || !nvram->init() || !nvram->attach(resources)) {
        IOLog("SwitchNVRAM: publish failed\n");
        OSSafeReleaseNULL(nvram);
        return status;
    }
    if (!nvram->start(resources)) {
        nvram->detach(resources);
        nvram->release();
        return status;
    }
    // IONVRAMController::registerService hands us to IODTNVRAM (which reads the current bank header) and publishes the
    // "IONVRAM" resource. The registry keeps the controller; gSwitchNVRAM is that reference.
    nvram->registerService();
    gSwitchNVRAM = nvram;
    nvram->release();
    status = 0;
    return status;
}

// SwitchBluetooth's strong definition wins when it is linked (../SwitchBluetooth/SwitchBluetooth.h).
extern "C" __attribute__((weak)) void switch_bluetooth_data_mounted(void)
{}

extern "C" uint32_t switch_nvram_data_mounted(void)
{
    const uint32_t added = SwitchNVRAM::restoreFromData();
    switch_bluetooth_data_mounted();
    return added;
}

extern "C" int switch_nvram_flush(void)
{
    return gSwitchNVRAM ? gSwitchNVRAM->flush(false) : 0;
}
