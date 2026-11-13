// SPDX-License-Identifier: MIT
// See raw_remote.h. The call forms are raw_kernel.cpp's (direct, or tunnelled through the selector the sandbox admits).
#include "raw_lock.h"
#include "raw_remote.h"

#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <pthread/qos.h>
#include <cstdio>
#include <cstring>
#include "TegraGM20BABI.h"
#include "TegraGM20BCompilerABI.h"

namespace raw {
namespace {

constexpr size_t kTunnelBytes = 64;
constexpr size_t kIdentityStackAt = 40;  // SwitchSeedStore.h: the header's identity, whose first word is the stack

}  // namespace

Remote::Remote(const KernelTransport &transport, uint64_t stack) : Remote(transportCall(transport), stack)
{}

Remote::Remote(Call kernel, uint64_t stack) : call_(std::move(kernel)), stack_(stack)
{
    TegraGM20BAbiCompilerInfoReply info = {};
    capable_ = call(TegraGM20BMethodCompilerInfo, nullptr, 0, &info, sizeof info) == kIOReturnSuccess &&
               (info.flags & TegraGM20BCompilerCapable);
    if (capable_)
        refresh(info);
}

void Remote::refresh(const TegraGM20BAbiCompilerInfoReply &info)
{
    if (info.flags & (TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered))
        expected_.store(true, std::memory_order_release);
    declined_.store(info.flags & TegraGM20BCompilerDeclined, std::memory_order_release);
    if ((info.flags & TegraGM20BCompilerStore) && info.stack == stack_)
        map(info.generation);
}

// No compiler took a request: whether one is registered or waited for now
void Remote::recheck()
{
    TegraGM20BAbiCompilerInfoReply info = {};
    if (call(TegraGM20BMethodCompilerInfo, nullptr, 0, &info, sizeof info) != kIOReturnSuccess)
        return;
    absent_.store(!(info.flags & (TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered)),
                  std::memory_order_release);
    refresh(info);
}

bool Remote::taking()
{
    if (absent_.load(std::memory_order_acquire))
        recheck();
    return !absent_.load(std::memory_order_acquire);
}

bool Remote::serving()
{
    if (!capable_)
        return false;
    TegraGM20BAbiCompilerInfoReply info = {};
    if (!expected_.load(std::memory_order_acquire) &&
        call(TegraGM20BMethodCompilerInfo, nullptr, 0, &info, sizeof info) == kIOReturnSuccess)
        refresh(info);
    return expected_.load(std::memory_order_acquire);
}

bool Remote::declined()
{
    TegraGM20BAbiCompilerInfoReply info = {};
    if (capable_ && call(TegraGM20BMethodCompilerInfo, nullptr, 0, &info, sizeof info) == kIOReturnSuccess)
        refresh(info);
    return declined_.load(std::memory_order_acquire);
}

Remote::~Remote()
{
    std::map<uint64_t, std::function<void(Answer)>> pending;
    {
        std::lock_guard<raw::Mutex> l(asyncLock_);
        if (port_)
            IONotificationPortDestroy(static_cast<IONotificationPortRef>(port_));
        port_ = nullptr;
        pending.swap(pending_);
    }
    for (auto &[cookie, done] : pending)
        done(Answer::Unavailable);
    if (queue_)
        dispatch_release(static_cast<dispatch_queue_t>(queue_));
}

Remote::Call Remote::transportCall(const KernelTransport &transport)
{
    return [transport](uint32_t method, const void *in, size_t inSize, void *out, size_t outSize, uint64_t *reference,
                       uint32_t port) -> int {
        if (!transport.connection)
            return kIOReturnNotOpen;
        alignas(uint64_t) uint8_t tunneled[kTunnelBytes];
        if (transport.tunnel) {
            if (inSize > sizeof tunneled - 8)
                return kIOReturnBadArgument;
            const uint32_t prefix[] = {TegraGM20BTunnelMagic, method};
            memcpy(tunneled, prefix, sizeof prefix);
            if (inSize)
                memcpy(tunneled + sizeof prefix, in, inSize);
            in = tunneled;
            inSize += sizeof prefix;
            method = transport.selector;
        }
        size_t outBytes = outSize;
        if (reference)
            return IOConnectCallAsyncStructMethod(transport.connection, method, port, reference, kOSAsyncRef64Count, in,
                                                  inSize, nullptr, nullptr);
        return IOConnectCallStructMethod(transport.connection, method, in, inSize, outSize ? out : nullptr,
                                         outSize ? &outBytes : nullptr);
    };
}

int Remote::call(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize, uint64_t *reference)
{
    const uint32_t port =
        reference && port_ ? IONotificationPortGetMachPort(static_cast<IONotificationPortRef>(port_)) : 0;
    return call_(method, in, inSize, out, outSize, reference, port);
}

// The store a compiler registered, checked to be this build's (its pack rules are the compiler's: both load the one
// installed pack)
void Remote::map(uint32_t generation)
{
    std::lock_guard<raw::Mutex> l(mapLock_);
    const Mapping *current = mapping_.load(std::memory_order_acquire);
    if (current && current->generation >= generation)
        return;
    const TegraGM20BAbiMap in = {TegraGM20BCompilerStoreToken, TegraGM20BMapReadOnly, 0};
    TegraGM20BAbiMapReply reply = {};
    if (call(TegraGM20BMethodMap, &in, sizeof in, &reply, sizeof reply) != kIOReturnSuccess)
        return;
    const auto *base = reinterpret_cast<const uint8_t *>(uintptr_t(reply.address));
    auto *made = new Mapping;
    made->generation = generation;
    made->base = base;
    made->bytes = reply.bytes;
    if (reply.bytes >= switch_seed_pack::kStoreHeaderBytes)
        memcpy(made->identity, base + kIdentityStackAt, sizeof made->identity);
    if (reply.bytes < switch_seed_pack::kStoreHeaderBytes ||
        switch_seed_pack::le64(base + kIdentityStackAt) != stack_ ||
        !made->reader.attach(base, size_t(reply.bytes), nullptr)) {
        delete made;
        return;
    }
    mappings_.push_back(made);
    mapping_.store(made, std::memory_order_release);
}

bool Remote::find(const Digest &name, std::string &bytes)
{
    const uint32_t latest = latest_.load(std::memory_order_acquire);
    const Mapping *m = mapping_.load(std::memory_order_acquire);
    if (latest && (!m || m->generation < latest)) {
        map(latest);
        m = mapping_.load(std::memory_order_acquire);
    }
    switch_seed_pack::Bytes found;
    if (!m || !m->reader.find(name.data(), found))
        return false;
    bytes.assign(reinterpret_cast<const char *>(found.data), found.size);
    return true;
}

const uint8_t *Remote::findSlot(const uint8_t *slot, size_t size, const uint8_t *&mapping, uint64_t &bytes) const
{
    const Mapping *m = mapping_.load(std::memory_order_acquire);
    if (!m)
        return nullptr;
    const Digest name = programSlotName(slot, size);
    switch_seed_pack::Bytes found;
    if (!m->reader.find(name.data(), found) || found.size < size || memcmp(found.data, slot, size))
        return nullptr;
    mapping = m->base;
    bytes = m->bytes;
    return found.data;
}

const uint8_t *Remote::storeMapping(uint64_t &bytes) const
{
    const Mapping *m = mapping_.load(std::memory_order_acquire);
    bytes = m ? m->bytes : 0;
    return m ? m->base : nullptr;
}

std::string Remote::storeIdentity() const
{
    const Mapping *m = mapping_.load(std::memory_order_acquire);
    if (!m)
        return std::string();
    std::string text;
    char hex[3];
    for (size_t i = 0; i < switch_seed_pack::kIdentityBytes; ++i) {
        snprintf(hex, sizeof hex, "%02x", m->identity[i]);
        text += hex;
    }
    return text;
}

Remote::Answer Remote::answerOf(int32_t result)
{
    switch (result) {
    case TegraGM20BCompileMade:
        return Answer::Made;
    case TegraGM20BCompileRefused:
        return Answer::Refused;
    case TegraGM20BCompileFailed:
        return Answer::Failed;
    default:
        return Answer::Unavailable;
    }
}

Remote::Answer Remote::compile(const std::string &request, uint32_t *how)
{
    if (!serving() || !taking())
        return Answer::Unavailable;
    const TegraGM20BAbiCompile in = {uint64_t(uintptr_t(request.data())), request.size(),         0, stack_,
                                     uint32_t(qos_class_self()),          TegraGM20BCompileWaited};
    TegraGM20BAbiCompileReply reply = {};
    const auto unanswered = [](int called) {
        return called == kIOReturnAborted || called == kIOReturnNoResources;
    };
    int called = call(TegraGM20BMethodCompile, &in, sizeof in, &reply, sizeof reply);
    // a wait a signal broke, or the connection's bound on requests in flight: asked once more
    if (unanswered(called))
        called = call(TegraGM20BMethodCompile, &in, sizeof in, &reply, sizeof reply);
    if (unanswered(called))
        return Answer::Failed;
    if (called != kIOReturnSuccess)
        return Answer::Unavailable;
    latest_.store(reply.generation, std::memory_order_release);
    if (how)
        *how = reply.how;
    if (reply.result == TegraGM20BCompileUnavailable)
        recheck();
    return answerOf(reply.result);
}

bool Remote::compileAsync(const std::string &request, bool waited, uint32_t qos, std::function<void(Answer)> done)
{
    if (!serving() || !taking())
        return false;
    std::lock_guard<raw::Mutex> l(asyncLock_);
    if (!port_) {
        IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
        if (!port)
            return false;
        queue_ = dispatch_queue_create(
            "com.brunostjohn.switch.raw.remote",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
        IONotificationPortSetDispatchQueue(port, static_cast<dispatch_queue_t>(queue_));
        port_ = port;
    }
    return ask({request, waited, qos, std::move(done)});
}

bool Remote::ask(Held &&held)
{
    const uint64_t cookie = nextCookie_++;
    const TegraGM20BAbiCompile in = {uint64_t(uintptr_t(held.request.data())),
                                     held.request.size(),
                                     cookie,
                                     stack_,
                                     held.waited ? held.qos : uint32_t(QOS_CLASS_BACKGROUND),
                                     held.waited ? uint32_t(TegraGM20BCompileWaited) : 0u};
    uint64_t reference[kOSAsyncRef64Count] = {};
    reference[kIOAsyncCalloutFuncIndex] = uint64_t(reinterpret_cast<uintptr_t>(&delivered));
    reference[kIOAsyncCalloutRefconIndex] = uint64_t(reinterpret_cast<uintptr_t>(this));
    pending_.emplace(cookie, held.done);
    const int asked = call(TegraGM20BMethodCompile, &in, sizeof in, nullptr, 0, reference);
    if (asked == kIOReturnSuccess)
        return true;
    pending_.erase(cookie);
    if (asked == kIOReturnNotReady)
        recheck();
    if (asked != kIOReturnNoResources || pending_.empty())
        return false;
    held_.push_back(std::move(held));
    return true;
}

const char *InProcess::text(Why why)
{
    switch (why) {
    case NoCompiler:
        return "no shader compiler runs this boot";
    }
    return "?";
}

void Remote::delivered(void *refcon, int result, void *cookie, void *word)
{
    auto *remote = static_cast<Remote *>(refcon);
    const uint64_t packed = uint64_t(uintptr_t(word));
    std::function<void(Answer)> done;
    std::vector<std::function<void(Answer)>> refused;
    {
        std::lock_guard<raw::Mutex> l(remote->asyncLock_);
        const auto found = remote->pending_.find(uint64_t(uintptr_t(cookie)));
        if (found == remote->pending_.end())
            return;
        done = std::move(found->second);
        remote->pending_.erase(found);
        if (!remote->held_.empty()) {
            Held next = std::move(remote->held_.front());
            remote->held_.erase(remote->held_.begin());
            std::function<void(Answer)> nextDone = next.done;
            if (!remote->ask(std::move(next)))
                refused.push_back(std::move(nextDone));
        }
    }
    for (const auto &r : refused)
        r(Answer::Unavailable);
    if (result != kIOReturnSuccess)
        return done(Answer::Unavailable);
    remote->latest_.store(uint32_t(packed >> 32), std::memory_order_release);
    const int32_t answer = int32_t(uint32_t(packed));
    if (answer == TegraGM20BCompileUnavailable)
        remote->recheck();
    done(answerOf(answer));
}

}  // namespace raw
