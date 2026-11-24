// SPDX-License-Identifier: MIT
// See kernel.h. The transport (direct call, or tunnel {magic, method} + record through the selector that answers the tunnel
// probe) is nvkmd_darwin's (out/gpu/nvk/nvk-darwin.patch, MIT).
#include "lock.h"
#include "kernel.h"
#include "TegraGM20BClockABI.h"
#include "TegraGM20BVolatileABI.h"
#include "encode_profile.h"

#include <IOKit/IOKitLib.h>
#include <os/log.h>
#include <mach/mach.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gm20b {
namespace {
constexpr uint64_t kVaGranule = 0x10000;
constexpr uint64_t kBigPageAlignment = 0x10000;
}  // namespace

Kernel::~Kernel()
{
    if (events_)
        events_->shutdown();
    if (conn_)
        IOServiceClose(conn_);
}

std::shared_ptr<SharedEvents> Kernel::sharedEvents()
{
    if (!exactJobs())
        return nullptr;
    std::lock_guard<gm20b::Mutex> lock(eventsLock_);
    if (!events_)
        events_ = SharedEvents::create(conn_, tunnel_, selector_);
    return events_;
}

static std::string failed(const char *what, int kr)
{
    char line[64];
    snprintf(line, sizeof line, "%s failed (0x%x)", what, unsigned(kr));
    return line;
}

int Kernel::call(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize)
{
    SWITCH_METAL_ENCODE_SPAN(Kernel);
    size_t outBytes = outSize;
    if (!tunnel_)
        return IOConnectCallStructMethod(conn_, method, in, inSize, outSize ? out : nullptr,
                                         outSize ? &outBytes : nullptr);
    uint32_t buffer[1024];
    if (inSize > sizeof buffer - 8)
        return kIOReturnBadArgument;
    buffer[0] = TegraGM20BTunnelMagic;
    buffer[1] = method;
    if (inSize)
        memcpy(&buffer[2], in, inSize);
    return IOConnectCallStructMethod(conn_, selector_, buffer, 8 + inSize, outSize ? out : nullptr,
                                     outSize ? &outBytes : nullptr);
}

bool Kernel::open(std::string &err)
{
    // ours (drivers/TegraGM20B); the first IOAcceleratorES is the stub that only ends QuartzCore's accelerator poll
    // (drivers/SwitchAccelStub)
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"));
    if (!service) {
        err = "no AGXAccelerator";
        return false;
    }
    io_connect_t c = 0;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), TegraGM20BOpenType, &c);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        err = "IOServiceOpen failed";
        return false;
    }
    conn_ = c;
    kr = call(TegraGM20BMethodGetInfo, nullptr, 0, &info_, sizeof info_);
    if (kr != KERN_SUCCESS) {
        const uint32_t probe[2] = {TegraGM20BTunnelMagic, TegraGM20BTunnelProbe};
        for (uint32_t s = 0; s < 256 && !tunnel_; s++)
            if (IOConnectCallStructMethod(conn_, s, probe, sizeof probe, nullptr, nullptr) == KERN_SUCCESS) {
                tunnel_ = true;
                selector_ = s;
            }
        if (tunnel_)
            kr = call(TegraGM20BMethodGetInfo, nullptr, 0, &info_, sizeof info_);
    }
    if (kr != KERN_SUCCESS || info_.abi_version != TegraGM20BAbiVersion) {
        err = "GetInfo failed";
        return false;
    }
    volatileWrites_ = (info_.flags & TegraGM20BInfoVolatileWrites) != 0;
    const uint64_t start = (info_.va_start + kVaGranule - 1) & ~(kVaGranule - 1);
    uint64_t end = info_.va_end & ~(kVaGranule - 1);
    if (supportsPackImage() && end > TGM_PACK_WINDOW_VA)
        end = TGM_PACK_WINDOW_VA;
    if (end > start)
        vaFree_[start] = end - start;
    return true;
}

int Kernel::packImageMap(const TegraGM20BAbiPackCpuRange *cpuRanges, uint32_t cpuRangeCount,
                         TegraGM20BAbiPackImageMapReply &reply)
{
    reply = {};
    if (!supportsPackImage())
        return kIOReturnUnsupported;
    if (cpuRangeCount > TegraGM20BMaxPackCpuRanges || (cpuRangeCount && !cpuRanges))
        return kIOReturnBadArgument;
    TegraGM20BAbiPackImageMap in = {};
    in.cpu_range_count = cpuRangeCount;
    for (uint32_t i = 0; i < cpuRangeCount; ++i)
        in.cpu_ranges[i] = cpuRanges[i];
    return call(TegraGM20BMethodPackImageMap, &in, sizeof in, &reply, sizeof reply);
}

uint64_t Kernel::allocVa(uint64_t bytes, uint64_t align)
{
    std::lock_guard<gm20b::Mutex> lock(vaLock_);
    bytes = (bytes + kVaGranule - 1) & ~(kVaGranule - 1);
    const uint64_t minimumAlignment = bytes >= kBigPageAlignment ? kBigPageAlignment : kVaGranule;
    if (align < minimumAlignment)
        align = minimumAlignment;
    for (auto it = vaFree_.begin(); it != vaFree_.end(); ++it) {
        const uint64_t at = (it->first + align - 1) & ~(align - 1), end = it->first + it->second;
        if (at + bytes > end)
            continue;
        const uint64_t start = it->first;
        vaFree_.erase(it);
        if (at > start)
            vaFree_[start] = at - start;
        if (at + bytes < end)
            vaFree_[at + bytes] = end - at - bytes;
        vaUsed_ += bytes;
        return at;
    }
    return 0;
}

void Kernel::freeVa(uint64_t va, uint64_t bytes)
{
    std::lock_guard<gm20b::Mutex> lock(vaLock_);
    bytes = (bytes + kVaGranule - 1) & ~(kVaGranule - 1);
    if (!va || !bytes)
        return;
    vaUsed_ -= bytes;
    auto next = vaFree_.lower_bound(va);
    if (next != vaFree_.end() && va + bytes == next->first) {  // merge with the range after
        bytes += next->second;
        next = vaFree_.erase(next);
    }
    if (next != vaFree_.begin()) {  // and with the one before
        auto prev = std::prev(next);
        if (prev->first + prev->second == va) {
            prev->second += bytes;
            return;
        }
    }
    vaFree_[va] = bytes;
}

bool Kernel::bind(uint64_t va, uint64_t handle, uint64_t range, uint32_t kind, bool gpuVolatile)
{
    TegraGM20BAbiVmBind b = {};
    b.count = 1;
    b.ops[0] = {gpuVolatile && exactJobs() ? TegraGM20BBindMapVolatile : TegraGM20BBindMap, kind, handle, 0, va, range};
    const size_t in = offsetof(TegraGM20BAbiVmBind, ops) + sizeof(b.ops[0]);
    if (const int kr = call(TegraGM20BMethodVmBind, &b, in, nullptr, 0); kr != KERN_SUCCESS) {
        os_log_error(OS_LOG_DEFAULT, "SwitchMetal: VmBind of BO %llu, %llu bytes at VA %#llx kind %#x, failed: 0x%x",
                     (unsigned long long)handle, (unsigned long long)range, (unsigned long long)va, kind, kr);
        setError("VmBind failed");
        return false;
    }
    return true;
}

bool Kernel::alloc(uint64_t bytes, bool coherent, Bo &bo, uint32_t kind, uint64_t align, bool gpuVolatile, bool control,
                   bool cpu, uint64_t at, bool systemOnly)
{
    bytes = (bytes + info_.page_size - 1) & ~uint64_t(info_.page_size - 1);
    TegraGM20BAbiBoAlloc a = {bytes, 12, coherent ? uint32_t(TegraGM20BBoCoherent) : 0u};
    if (control && coherent && !kind)
        a.flags |= TegraGM20BBoControl;
    if (systemOnly && coherent && !control)
        a.flags |= TegraGM20BBoSystem;
    TegraGM20BAbiBoAllocReply r = {};
    bo = {};
    if (call(TegraGM20BMethodBoAlloc, &a, sizeof a, &r, sizeof r) != KERN_SUCCESS) {
        setError("BoAlloc failed");
        return false;
    }
    bo.handle = r.handle;
    bo.token = r.map_token;
    bo.bytes = bytes;
    bo.allocated = true;
    allocated_.fetch_add(bytes, std::memory_order_relaxed);
    uint64_t addr = 0, size = 0;
    if (cpu && (!map(r.map_token, false, addr, size) || size < bytes)) {
        setError("BO map failed");
        free(bo);
        return false;
    }
    bo.cpu = reinterpret_cast<uint8_t *>(uintptr_t(addr));
    bo.mapped = size;
    bo.control = control;
    if (control)
        controlMapped_.fetch_add(size, std::memory_order_relaxed);
    bo.va = bo.vaBase = at ? at : allocVa(bytes, align);
    bo.vaReserved = at != 0;
    if (!bo.va) {
        setError("out of GPU VA");
        free(bo);
        return false;
    }
    bo.gpuVolatile = gpuVolatile && exactJobs();
    if (!bind(bo.va, bo.handle, bytes, kind, bo.gpuVolatile)) {
        free(bo);
        return false;
    }
    return true;
}

bool Kernel::import(void *addr, uint64_t bytes, Bo &bo, uint32_t kind, uint32_t surfaceId)
{
    if (!importPages(addr, bytes, bo, surfaceId)) {
        setError("BoImport failed");
        return false;
    }
    return bindImport(addr, bo, kind);
}

bool Kernel::importPages(void *addr, uint64_t bytes, Bo &bo, uint32_t surfaceId, bool gpuReads)
{
    const uint64_t page = info_.page_size;
    const uintptr_t base = uintptr_t(addr) & ~uintptr_t(page - 1);
    const uint64_t span = ((uintptr_t(addr) + bytes + page - 1) & ~uintptr_t(page - 1)) - base;
    gpuReads = gpuReads || (surfaceId && !importsWritten_.load(std::memory_order_relaxed));
    TegraGM20BAbiBoImport in = {base, span, surfaceId, gpuReads ? uint32_t(TegraGM20BImportGpuReads) : 0};
    TegraGM20BAbiBoAllocReply r = {};
    bo = {};
    if (const int kr = call(TegraGM20BMethodBoImport, &in, sizeof in, &r, sizeof r); kr != KERN_SUCCESS) {
        os_log_error(OS_LOG_DEFAULT, "SwitchMetal: BoImport of %llu bytes at %p (IOSurface %u) failed: 0x%x",
                     (unsigned long long)span, (void *)base, surfaceId, kr);
        return false;
    }
    bo.handle = r.handle;
    bo.bytes = span;
    return true;
}

bool Kernel::surfaceGpuWrites(uint32_t surfaceId)
{
    const TegraGM20BAbiSurfaceWrites in = {surfaceId, 0};
    if (call(TegraGM20BMethodSurfaceWrites, &in, sizeof in, nullptr, 0) == KERN_SUCCESS)
        return true;
    importsWritten_.store(true, std::memory_order_relaxed);
    return false;
}

bool Kernel::priority(uint32_t priority, uint32_t background)
{
    if (!(info_.flags & TegraGM20BInfoPriority))
        return false;
    const TegraGM20BAbiPriority in = {priority, background};
    return call(TegraGM20BMethodPriority, &in, sizeof in, nullptr, 0) == KERN_SUCCESS;
}

uint32_t Kernel::surfaceLayout(const Bo &bo, uint32_t width, uint32_t height, uint32_t bpp, uint32_t bytesPerRow,
                               bool writes, tgm_layout *canonical, uint32_t *kind)
{
    if (kind)
        *kind = 0;
    const TegraGM20BAbiSurfaceLayout in = {
        bo.handle, width, height, bpp, bytesPerRow, writes ? uint32_t(TegraGM20BLayoutWrites) : 0, 0};
    if (canonical && (info_.flags & TegraGM20BInfoCanonicalSurfaceLayout)) {
        TegraGM20BAbiSurfaceLayoutInfo out = {};
        const int kr = call(TegraGM20BMethodSurfaceLayoutInfo, &in, sizeof in, &out, sizeof out);
        if (kr != KERN_SUCCESS) {
            os_log_error(
                OS_LOG_DEFAULT,
                "SwitchMetal: SurfaceLayoutInfo of import %llu (%ux%u x%u, rows %u) failed: 0x%x: no texture over it",
                (unsigned long long)bo.handle, width, height, bpp, bytesPerRow, kr);
            return TegraGM20BLayoutOtherShape;
        }
        if (out.layout == TegraGM20BLayoutTiled) {
            *canonical = {out.width, out.height,          out.bpp,   out.pitch,
                          out.rows,  uint8_t(out.y_log2), out.align, out.bytes};
            if (kind)
                *kind = out.kind ? out.kind : TGM_LAYOUT_KIND;
        } else if (out.layout == TegraGM20BLayoutOtherShape) {
            os_log_error(
                OS_LOG_DEFAULT,
                "SwitchMetal: SurfaceLayoutInfo of import %llu (%ux%u x%u, rows %u): laid out for another shape: no "
                "texture over it",
                (unsigned long long)bo.handle, width, height, bpp, bytesPerRow);
        }
        return out.layout;
    }
    if (!(info_.flags & TegraGM20BInfoSurfaceLayout))
        return 0;
    TegraGM20BAbiSurfaceLayoutReply out = {};
    if (const int kr = call(TegraGM20BMethodSurfaceLayout, &in, sizeof in, &out, sizeof out); kr != KERN_SUCCESS) {
        os_log_error(
            OS_LOG_DEFAULT,
            "SwitchMetal: SurfaceLayout of import %llu (%ux%u x%u, rows %u, %s) failed: 0x%x: no texture over it",
            (unsigned long long)bo.handle, width, height, bpp, bytesPerRow, writes ? "writes" : "reads", kr);
        return TegraGM20BLayoutOtherShape;
    }
    if (canonical && out.layout == TegraGM20BLayoutTiled && !tgm_layout_tiled(width, height, bpp, canonical)) {
        os_log_error(
            OS_LOG_DEFAULT,
            "SwitchMetal: SurfaceLayout of import %llu (%ux%u x%u): tiled at a shape NIL lays out none of: no texture "
            "over it",
            (unsigned long long)bo.handle, width, height, bpp);
        return TegraGM20BLayoutOtherShape;
    }
    if (out.layout == TegraGM20BLayoutTiled && kind)
        *kind = TGM_LAYOUT_KIND;
    if (out.layout == TegraGM20BLayoutOtherShape)
        os_log_error(
            OS_LOG_DEFAULT,
            "SwitchMetal: SurfaceLayout of import %llu (%ux%u x%u, rows %u, %s): tiled at another shape: no texture "
            "over it",
            (unsigned long long)bo.handle, width, height, bpp, bytesPerRow, writes ? "writes" : "reads");
    return out.layout;
}

int Kernel::nativeSurfaceView(const Bo &source, Bo &view, TegraGM20BAbiNativeSurfaceViewReply &layout)
{
    layout = {};
    view = {};
    if (!exactJobs())
        return KERN_SUCCESS;
    const TegraGM20BAbiNativeSurfaceView in = {source.handle, 0, 0};
    const int result = call(TegraGM20BMethodNativeSurfaceView, &in, sizeof in, &layout, sizeof layout);
    if (result != KERN_SUCCESS || !layout.handle)
        return result;
    view.handle = layout.handle;
    view.bytes = layout.bytes;
    if (!bindImport(nullptr, view, layout.kind))
        return kIOReturnNoResources;
    return KERN_SUCCESS;
}

bool Kernel::bindImport(void *addr, Bo &bo, uint32_t kind)
{
    const uintptr_t base = uintptr_t(addr) & ~uintptr_t(info_.page_size - 1);
    bo.vaBase = allocVa(bo.bytes, kVaGranule);
    if (!bo.vaBase) {
        os_log_error(OS_LOG_DEFAULT,
                     "SwitchMetal: no GPU VA left for an import of %llu bytes (%llu held): no texture over it",
                     (unsigned long long)bo.bytes, (unsigned long long)vaUsed_.load(std::memory_order_relaxed));
        setError("out of GPU VA");
        free(bo);
        return false;
    }
    if (!bind(bo.vaBase, bo.handle, bo.bytes, kind)) {
        free(bo);
        return false;
    }
    bo.va = bo.vaBase + (uintptr_t(addr) - base);
    bo.cpu = static_cast<uint8_t *>(addr);
    return true;
}

uint64_t Kernel::reserveVa(uint64_t bytes)
{
    return allocVa(bytes, kVaGranule);
}

void Kernel::releaseVa(uint64_t va, uint64_t bytes)
{
    freeVa(va, bytes);
}

bool Kernel::bindImportAt(void *addr, Bo &bo, uint64_t va, uint32_t kind)
{
    if (!bind(va, bo.handle, bo.bytes, kind)) {
        free(bo);
        return false;
    }
    bo.vaBase = va;
    bo.vaReserved = true;
    bo.va = va + (uintptr_t(addr) & (info_.page_size - 1));
    bo.cpu = static_cast<uint8_t *>(addr);
    return true;
}

bool Kernel::bindAlias(const Bo &bo, uint64_t va, uint32_t kind)
{
    return bind(va, bo.handle, bo.bytes, kind);
}

bool Kernel::bindWindow(const Bo &bo, uint64_t va, uint32_t kind, uint64_t bytes)
{
    TegraGM20BAbiVmBind b = {};
    b.count = 2;
    b.ops[0] = {TegraGM20BBindMap, kind, bo.handle, 0, va, bo.bytes};
    b.ops[1] = {TegraGM20BBindMapSparse, 0, 0, 0, va + bo.bytes, bytes - bo.bytes};
    const size_t in = offsetof(TegraGM20BAbiVmBind, ops) + 2 * sizeof(b.ops[0]);
    if (const int kr = call(TegraGM20BMethodVmBind, &b, in, nullptr, 0); kr != KERN_SUCCESS) {
        os_log_error(OS_LOG_DEFAULT,
                     "SwitchMetal: VmBind of BO %llu at VA %#llx and a sparse tail to %llu bytes failed: 0x%x",
                     (unsigned long long)bo.handle, (unsigned long long)va, (unsigned long long)bytes, kr);
        setError("VmBind of a sparse window failed");
        return false;
    }
    return true;
}

bool Kernel::bindSparse(uint64_t va, uint64_t bytes)
{
    TegraGM20BAbiVmBind b = {};
    b.count = 1;
    b.ops[0] = {TegraGM20BBindMapSparse, 0, 0, 0, va, bytes};
    const size_t in = offsetof(TegraGM20BAbiVmBind, ops) + sizeof(b.ops[0]);
    if (const int kr = call(TegraGM20BMethodVmBind, &b, in, nullptr, 0); kr != KERN_SUCCESS) {
        os_log_error(OS_LOG_DEFAULT, "SwitchMetal: VmBind of %llu sparse bytes at VA %#llx failed: 0x%x",
                     (unsigned long long)bytes, (unsigned long long)va, kr);
        setError("VmBind of a sparse range failed");
        return false;
    }
    return true;
}

void Kernel::unbind(uint64_t va, uint64_t range)
{
    TegraGM20BAbiVmBind b = {};
    b.count = 1;
    b.ops[0] = {TegraGM20BBindUnmap, 0, 0, 0, va, range};
    const size_t in = offsetof(TegraGM20BAbiVmBind, ops) + sizeof(b.ops[0]);
    if (const int kr = call(TegraGM20BMethodVmBind, &b, in, nullptr, 0); kr != KERN_SUCCESS)
        os_log_error(OS_LOG_DEFAULT, "SwitchMetal: VmBind Unmap of %llu bytes at VA %#llx failed: 0x%x",
                     (unsigned long long)range, (unsigned long long)va, kr);
}

void Kernel::free(Bo &bo)
{
    const bool listed = bo.handle && freeObserver_ && freeObserver_->leaving(bo.handle);
    if (bo.token && bo.cpu && !bo.parked)
        unmap(bo.token, uint64_t(uintptr_t(bo.cpu)));
    if (bo.handle) {
        TegraGM20BAbiBoFree f = {bo.handle};
        call(TegraGM20BMethodBoFree, &f, sizeof f, nullptr, 0);
    }
    if (bo.allocated)
        allocated_.fetch_sub(bo.bytes, std::memory_order_relaxed);
    if (bo.control)
        controlMapped_.fetch_sub(bo.mapped, std::memory_order_relaxed);
    if (bo.vaBase && !bo.vaReserved)
        freeVa(bo.vaBase, bo.bytes);
    bo = {};
    if (listed)
        freeObserver_->left(*this);
}

int Kernel::park(uint64_t handle, bool guardNone)
{
    if (!supportsBoPark())
        return kIOReturnUnsupported;
    if (!handle)
        return kIOReturnBadArgument;
    const TegraGM20BAbiBoPark in = {handle, guardNone ? uint32_t(TegraGM20BParkGuard) : 0u, 0};
    return call(TegraGM20BMethodBoPark, &in, sizeof in, nullptr, 0);
}

int Kernel::unpark(uint64_t handle, uint64_t &address, uint64_t &bytes, uint32_t &status)
{
    address = 0;
    bytes = 0;
    status = 0;
    if (!supportsBoPark())
        return kIOReturnUnsupported;
    if (!handle)
        return kIOReturnBadArgument;
    const TegraGM20BAbiBoUnpark in = {handle, 0, 0};
    TegraGM20BAbiBoUnparkReply r = {};
    const int kr = call(TegraGM20BMethodBoUnpark, &in, sizeof in, &r, sizeof r);
    if (kr == KERN_SUCCESS) {
        address = r.address;
        bytes = r.bytes;
        status = r.status;
    }
    return kr;
}

bool Kernel::mapAt(uint32_t token, uint64_t bytes, uint64_t fixedVa, bool readOnly)
{
    if (!(info_.flags & TegraGM20BInfoKernelMap) || !token || !fixedVa)
        return false;
    const TegraGM20BAbiMap in = {
        token, uint32_t(TegraGM20BMapFixed) | (readOnly ? uint32_t(TegraGM20BMapReadOnly) : 0u), fixedVa};
    TegraGM20BAbiMapReply r = {};
    if (call(TegraGM20BMethodMap, &in, sizeof in, &r, sizeof r) != KERN_SUCCESS)
        return false;
    if (r.address != fixedVa || r.bytes < bytes) {
        unmap(token, r.address);
        return false;
    }
    return true;
}

bool Kernel::channel(uint32_t engines, uint32_t &channel, const volatile uint32_t *&fence)
{
    std::lock_guard<gm20b::Mutex> l(lostLock_);
    if (engines == TegraGM20BEngineCopy)
        engines |= TegraGM20BEngineCopyOnly;
    TegraGM20BAbiChannelCreate in = {engines, 0};
    TegraGM20BAbiChannelCreateReply r = {};
    if (const int kr = call(TegraGM20BMethodChannelCreate, &in, sizeof in, &r, sizeof r); kr != KERN_SUCCESS) {
        setError(failed("ChannelCreate", kr));
        return false;
    }
    if (!fenceMap_ && !map(r.fence_token, true, fenceMap_, fenceMapBytes_)) {
        fenceMap_ = 0;
        fenceMapBytes_ = 0;
        closeChannel(r.channel);
        setError("fence map failed");
        return false;
    }
    const uint64_t recordBytes =
        sizeof(TegraGM20BAbiFence) + sizeof(TegraGM20BAbiFenceSemantic) + sizeof(TegraGM20BAbiFenceIdentity);
    if ((r.fence_offset & 7) || r.fence_offset > fenceMapBytes_ || recordBytes > fenceMapBytes_ - r.fence_offset) {
        closeChannel(r.channel);
        setError("fence record outside mapped range");
        return false;
    }
    channel = r.channel;
    fence = reinterpret_cast<const volatile uint32_t *>(uintptr_t(fenceMap_) + r.fence_offset);
    opened_.fetch_add(1, std::memory_order_release);
    uint32_t reused = r.channel;
    lostChannel_.compare_exchange_strong(reused, 0, std::memory_order_relaxed);
    return true;
}

void Kernel::closeChannel(uint32_t channel)
{
    const TegraGM20BAbiChannel in = {channel, 0};
    call(TegraGM20BMethodChannelDestroy, &in, sizeof in, nullptr, 0);
}

const volatile uint32_t *Kernel::channelEpoch()
{
    std::lock_guard<gm20b::Mutex> l(lostLock_);
    if (!fenceMap_ && !map(info_.fence_token, true, fenceMap_, fenceMapBytes_))
        return nullptr;
    return reinterpret_cast<const volatile uint32_t *>(uintptr_t(fenceMap_)) + TegraGM20BChannelEpochWord;
}

// A clientMemoryForType token mapped into this task: through the kext's own Map (method 12) where it has one, as NVK's
// nvkmd_darwin does then (a sandbox may admit io_connect_method and not io_connect_map_memory_into_task), else IOKit's.
bool Kernel::map(uint32_t token, bool readOnly, uint64_t &address, uint64_t &bytes)
{
    if (info_.flags & TegraGM20BInfoKernelMap) {
        const TegraGM20BAbiMap in = {token, readOnly ? uint32_t(TegraGM20BMapReadOnly) : 0u, 0};
        TegraGM20BAbiMapReply r = {};
        if (call(TegraGM20BMethodMap, &in, sizeof in, &r, sizeof r) != KERN_SUCCESS)
            return false;
        address = r.address;
        bytes = r.bytes;
        return true;
    }
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    if (IOConnectMapMemory64(conn_, token, mach_task_self(), &addr, &size,
                             kIOMapAnywhere | (readOnly ? kIOMapReadOnly : 0)) != KERN_SUCCESS)
        return false;
    address = addr;
    bytes = size;
    return true;
}

void Kernel::unmap(uint32_t token, uint64_t address)
{
    if (info_.flags & TegraGM20BInfoKernelMap) {
        const TegraGM20BAbiUnmap in = {token, 0, address};
        call(TegraGM20BMethodUnmap, &in, sizeof in, nullptr, 0);
        return;
    }
    IOConnectUnmapMemory64(conn_, token, mach_task_self(), mach_vm_address_t(address));
}

bool Kernel::cpuAccess(const Bo &bo, uint64_t offset, uint64_t bytes, uint32_t access, bool acquire)
{
    if (!(info_.flags & TegraGM20BInfoCpuAccess) || !bytes || offset > bo.bytes || bytes > bo.bytes - offset) {
        setError("CPU ownership unavailable or invalid range");
        return false;
    }
    const TegraGM20BAbiCpuAccess in = {bo.handle, offset, bytes, access, 0};
    const int kr = call(acquire ? TegraGM20BMethodCpuAcquire : TegraGM20BMethodCpuRelease, &in, sizeof in, nullptr, 0);
    if (kr != KERN_SUCCESS)
        setError(failed(acquire ? "CpuAcquire" : "CpuRelease", kr));
    return kr == KERN_SUCCESS;
}

CpuCompletionResult Kernel::completeCpu(const CpuCompletion &completion)
{
    return completeCpuRanges(completion, [&](const auto &request, auto &reply) {
        const int kr = call(TegraGM20BMethodCpuComplete, &request, sizeof request, &reply, sizeof reply);
        if (kr != KERN_SUCCESS)
            setError(failed("CpuComplete", kr));
        return kr == KERN_SUCCESS;
    });
}

// The kernel keeps a killed channel, and its cause, until ChannelDestroy (reopen), so GetStatus still answers for it here
bool Kernel::status(uint32_t channel, TegraGM20BAbiStatus &st)
{
    const TegraGM20BAbiChannel in = {channel, 0};
    st = {};
    if (call(TegraGM20BMethodGetStatus, &in, sizeof in, &st, sizeof st) == KERN_SUCCESS)
        return true;
    st = {};
    return false;
}

bool Kernel::clockAnchors(const volatile TegraGM20BAbiClockAnchors *&table, uint64_t &gpuVa)
{
    TegraGM20BAbiClockAnchorsReply r = {};
    if (call(TegraGM20BMethodClockAnchors, nullptr, 0, &r, sizeof r) != KERN_SUCCESS || !r.gpu_va)
        return false;
    std::lock_guard<gm20b::Mutex> l(lostLock_);
    if (!fenceMap_ && !map(r.fence_token, true, fenceMap_, fenceMapBytes_)) {
        fenceMap_ = 0;
        fenceMapBytes_ = 0;
        return false;
    }
    if (fenceMapBytes_ < TegraGM20BClockAnchorOffset + sizeof(TegraGM20BAbiClockAnchors))
        return false;
    table = reinterpret_cast<const volatile TegraGM20BAbiClockAnchors *>(uintptr_t(fenceMap_) +
                                                                         TegraGM20BClockAnchorOffset);
    gpuVa = r.gpu_va;
    return true;
}

// kIOReturnNotResponding is the kernel's answer for a channel it killed: one GetStatus still calls healthy is not lost
void Kernel::notResponding(uint32_t channel, uint32_t opened)
{
    TegraGM20BAbiStatus st;
    if (status(channel, st) && !st.error)
        return;
    markLost(channel, opened);
}

void Kernel::markLost(uint32_t channel, uint32_t opened)
{
    std::lock_guard<gm20b::Mutex> l(lostLock_);
    if (opened != opened_.load(std::memory_order_relaxed))
        return;
    TegraGM20BAbiStatus st;
    status(channel, st);  // none: the channel already destroyed (another thread's reopen), the cause unknown
    lostFaultVa_.store(st.fault_va, std::memory_order_relaxed);
    lostError_.store(st.error, std::memory_order_relaxed);
    lostChannel_.store(channel, std::memory_order_relaxed);
}

bool Kernel::wait(uint32_t channel, uint32_t seqno, uint64_t timeoutNs, int *result)
{
    TegraGM20BAbiWaitFence w = {channel, seqno, timeoutNs};
    const uint32_t at = opened();
    const int kr = call(TegraGM20BMethodWaitFence, &w, sizeof w, nullptr, 0);
    if (result)
        *result = kr;
    if (kr == kIOReturnNotResponding)
        notResponding(channel, at);
    if (kr != KERN_SUCCESS) {
        setError("WaitFence failed");
        return false;
    }
    return true;
}

}  // namespace gm20b
