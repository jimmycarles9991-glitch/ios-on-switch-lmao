// SPDX-License-Identifier: MIT
// See raw_kernel.h. The transport (direct call, or tunnel {magic, method} + record through the selector that answers the tunnel
// probe) is nvkmd_darwin's (out/gpu/nvk/nvk-darwin.patch, MIT), so whatever the kext admits for NVK it admits here.
#include "raw_kernel.h"

#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace raw {
namespace {
constexpr uint64_t kVaGranule = 0x10000;  // the GMMU big page: every range starts and ends on one
constexpr char kNvk[] = "libvulkan_nouveau.dylib";

// NVK's image: the one this process loaded already, else the one beside this image (the plugin's bundle, which the m2v device's
// open loads NVK from: the same image then). Null: none (a host tool of the raw path alone).
void *nvkImage()
{
    const size_t suffix = strlen(kNvk);
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        const size_t length = name ? strlen(name) : 0;
        if (length > suffix && name[length - suffix - 1] == '/' && !strcmp(name + length - suffix, kNvk))
            return dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    }
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&nvkImage), &info) || !info.dli_fname)
        return nullptr;
    std::string path(info.dli_fname);
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos)
        return nullptr;
    path.replace(slash + 1, std::string::npos, kNvk);
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}
}  // namespace

Kernel::~Kernel()
{
    if (release_)
        release_();
    else if (conn_)
        IOServiceClose(conn_);
}

static std::string failed(const char *what, int kr)
{
    char line[64];
    snprintf(line, sizeof line, "%s failed (0x%x)", what, unsigned(kr));
    return line;
}

int Kernel::call(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize)
{
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
    using Connection = bool (*)(uint32_t *, bool *, uint32_t *, uint64_t *, uint64_t *);
    void *nvk = nvkImage();
    if (!nvk)
        return openOwn(err);
    const auto connection = reinterpret_cast<Connection>(dlsym(nvk, "switch_nvk_connection"));
    const auto release = reinterpret_cast<void (*)()>(dlsym(nvk, "switch_nvk_connection_release"));
    if (!connection || !release) {
        // No fallback to a connection of our own: beside NVK's it would split the process's GPU VM in two
        err = "the loaded NVK has no switch_nvk_connection, so it is older than this tree (rebuild //gpu/nvk:host)";
        return false;
    }
    // [vaStart, vaEnd) is the whole window NVK leaves out, handed to every caller: one Kernel per process may take it (the
    // render server's one raw device), or two free lists would hand out the same VAs.
    uint64_t vaStart = 0, vaEnd = 0;
    if (!connection(&conn_, &tunnel_, &selector_, &vaStart, &vaEnd)) {
        err = "NVK's connection did not open";
        return false;
    }
    release_ = release;
    shared_ = true;
    if (call(TegraGM20BMethodGetInfo, nullptr, 0, &info_, sizeof info_) != KERN_SUCCESS ||
        info_.abi_version != TegraGM20BAbiVersion) {
        err = "GetInfo failed";
        return false;
    }
    vaStart = (vaStart + kVaGranule - 1) & ~(kVaGranule - 1);
    vaFree_[vaStart] = (vaEnd & ~(kVaGranule - 1)) - vaStart;
    return true;
}

bool Kernel::openOwn(std::string &err)
{
    const char *name = getenv("NVK_DARWIN_SERVICE");
    if (!name || !*name)
        name = "IOAcceleratorES";
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(name));
    if (!service) {
        err = std::string("no ") + name;
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
    // the top quarter of the client range
    const uint64_t start =
        (info_.va_start + (info_.va_end - info_.va_start) / 4 * 3 + kVaGranule - 1) & ~(kVaGranule - 1);
    vaFree_[start] = (info_.va_end & ~(kVaGranule - 1)) - start;
    return true;
}

uint64_t Kernel::allocVa(uint64_t bytes, uint64_t align)
{
    bytes = (bytes + kVaGranule - 1) & ~(kVaGranule - 1);
    if (align < kVaGranule)
        align = kVaGranule;
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

bool Kernel::bind(uint64_t va, uint64_t handle, uint64_t range, uint32_t kind)
{
    TegraGM20BAbiVmBind b = {};
    b.count = 1;
    b.ops[0] = {TegraGM20BBindMap, kind, handle, 0, va, range};
    const size_t in = offsetof(TegraGM20BAbiVmBind, ops) + sizeof(b.ops[0]);
    if (call(TegraGM20BMethodVmBind, &b, in, nullptr, 0) != KERN_SUCCESS) {
        err_ = "VmBind failed";
        return false;
    }
    return true;
}

bool Kernel::alloc(uint64_t bytes, bool coherent, Bo &bo, uint32_t kind, uint64_t align)
{
    bytes = (bytes + info_.page_size - 1) & ~uint64_t(info_.page_size - 1);
    TegraGM20BAbiBoAlloc a = {bytes, 12, coherent ? uint32_t(TegraGM20BBoCoherent) : 0u};
    TegraGM20BAbiBoAllocReply r = {};
    bo = {};
    if (call(TegraGM20BMethodBoAlloc, &a, sizeof a, &r, sizeof r) != KERN_SUCCESS) {
        err_ = "BoAlloc failed";
        return false;
    }
    bo.handle = r.handle;
    bo.token = r.map_token;
    bo.bytes = bytes;
    uint64_t addr = 0, size = 0;
    if (!map(r.map_token, false, addr, size) || size < bytes) {
        err_ = "BO map failed";
        free(bo);
        return false;
    }
    bo.cpu = reinterpret_cast<uint8_t *>(uintptr_t(addr));
    bo.mapped = size;
    bo.va = bo.vaBase = allocVa(bytes, align);
    if (!bo.va) {
        err_ = "out of GPU VA";
        free(bo);
        return false;
    }
    if (!bind(bo.va, bo.handle, bytes, kind)) {
        free(bo);
        return false;
    }
    return true;
}

bool Kernel::import(void *addr, uint64_t bytes, Bo &bo, uint32_t kind, uint32_t surfaceId)
{
    if (!importPages(addr, bytes, bo, surfaceId)) {
        err_ = "BoImport failed";
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
    if (call(TegraGM20BMethodBoImport, &in, sizeof in, &r, sizeof r) != KERN_SUCCESS)
        return false;
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

uint32_t Kernel::surfaceLayout(const Bo &bo, uint32_t width, uint32_t height, uint32_t bpp, uint32_t bytesPerRow,
                               bool writes)
{
    if (!(info_.flags & TegraGM20BInfoSurfaceLayout))
        return 0;
    const TegraGM20BAbiSurfaceLayout in = {
        bo.handle, width, height, bpp, bytesPerRow, writes ? uint32_t(TegraGM20BLayoutWrites) : 0, 0};
    TegraGM20BAbiSurfaceLayoutReply out = {};
    return call(TegraGM20BMethodSurfaceLayout, &in, sizeof in, &out, sizeof out) == KERN_SUCCESS ? out.layout : 0;
}

bool Kernel::bindImport(void *addr, Bo &bo, uint32_t kind)
{
    const uintptr_t base = uintptr_t(addr) & ~uintptr_t(info_.page_size - 1);
    bo.vaBase = allocVa(bo.bytes, kVaGranule);
    if (!bo.vaBase) {
        err_ = "out of GPU VA";
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
    bo.va = va + (uintptr_t(addr) & (info_.page_size - 1));
    bo.cpu = static_cast<uint8_t *>(addr);
    return true;
}

void Kernel::free(Bo &bo)
{
    if (bo.token && bo.cpu)
        unmap(bo.token, uint64_t(uintptr_t(bo.cpu)));
    if (bo.handle) {
        TegraGM20BAbiBoFree f = {bo.handle};
        call(TegraGM20BMethodBoFree, &f, sizeof f, nullptr, 0);
    }
    if (bo.vaBase)
        freeVa(bo.vaBase, bo.bytes);
    bo = {};
}

bool Kernel::channel(uint32_t engines, uint32_t &channel, const volatile uint32_t *&fence)
{
    TegraGM20BAbiChannelCreate in = {engines, 0};
    TegraGM20BAbiChannelCreateReply r = {};
    if (const int kr = call(TegraGM20BMethodChannelCreate, &in, sizeof in, &r, sizeof r); kr != KERN_SUCCESS) {
        err_ = failed("ChannelCreate", kr);
        return false;
    }
    uint64_t size = 0;
    if (!fenceMap_ && !map(r.fence_token, true, fenceMap_, size)) {
        fenceMap_ = 0;
        err_ = "fence map failed";
        return false;
    }
    channel = r.channel;
    fence = reinterpret_cast<const volatile uint32_t *>(uintptr_t(fenceMap_) + r.fence_offset);
    std::lock_guard<std::mutex> l(lostLock_);
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

int Kernel::order(uint32_t channel, uint32_t flags, uint32_t seqno, uint32_t &waits)
{
    const TegraGM20BAbiOrder in = {channel, flags, seqno, 0};
    TegraGM20BAbiOrderReply r = {};
    const uint32_t at = opened();
    const int kr = call(TegraGM20BMethodOrder, &in, sizeof in, &r, sizeof r);
    if (kr == kIOReturnNotResponding)
        markLost(channel, at);
    waits = kr == KERN_SUCCESS ? r.waits : 0;
    return kr;
}

bool Kernel::exec(uint32_t channel, const TegraGM20BAbiPush *pushes, uint32_t count, uint32_t last, uint32_t &seqno,
                  SurfaceList *surfaces)
{
    TegraGM20BAbiExec e;
    e.channel = channel;
    e.count = count;
    memcpy(e.pushes, pushes, count * sizeof pushes[0]);
    if (count && importsWritten_.load(std::memory_order_relaxed))
        e.pushes[0].flags |= TegraGM20BPushImportsWritten;
    const size_t in = offsetof(TegraGM20BAbiExec, pushes) + count * sizeof pushes[0];
    TegraGM20BAbiExecReply r = {};
    const bool listed = surfaces && (info_.flags & TegraGM20BInfoExecSurfaces);
    const uint32_t at = opened();
    for (;;) {
        if (listed) {
            size_t bytes = 0;
            const TegraGM20BAbiExecSurfaces &list = surfaces->record(channel, bytes);
            call(TegraGM20BMethodExecSurfaces, &list, bytes, nullptr, 0);
        }
        const int kr = call(TegraGM20BMethodExec, &e, in, &r, sizeof r);
        if (kr == KERN_SUCCESS) {
            seqno = r.seqno;
            return true;
        }
        if (kr == kIOReturnNotResponding)
            markLost(channel, at);
        if (kr != kIOReturnBusy || !last) {
            err_ = failed("Exec", kr);
            return false;
        }
        // GPFIFO full: block on the last submission's fence (interrupt-driven in the kext), never spin
        if (!wait(channel, last))
            return false;
    }
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

void Kernel::markLost(uint32_t channel, uint32_t opened)
{
    std::lock_guard<std::mutex> l(lostLock_);
    if (opened != opened_.load(std::memory_order_relaxed))
        return;
    TegraGM20BAbiStatus st;
    status(channel, st);  // none: the channel already destroyed (another thread's reopen), the cause unknown
    lostFaultVa_.store(st.fault_va, std::memory_order_relaxed);
    lostError_.store(st.error, std::memory_order_relaxed);
    lostChannel_.store(channel, std::memory_order_relaxed);
}

bool Kernel::wait(uint32_t channel, uint32_t seqno, uint64_t timeoutNs)
{
    TegraGM20BAbiWaitFence w = {channel, seqno, timeoutNs};
    const uint32_t at = opened();
    const int kr = call(TegraGM20BMethodWaitFence, &w, sizeof w, nullptr, 0);
    if (kr == kIOReturnNotResponding)
        markLost(channel, at);
    if (kr != KERN_SUCCESS) {
        err_ = "WaitFence failed";
        return false;
    }
    return true;
}

}  // namespace raw
