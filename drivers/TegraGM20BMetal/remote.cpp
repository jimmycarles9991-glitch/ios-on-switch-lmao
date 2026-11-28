// SPDX-License-Identifier: MIT
// See remote.h. The call forms are kernel.cpp's (direct, or tunnelled through the selector the sandbox admits).
#include "answer_class.h"
#include "lock.h"
#include "remote.h"
#include "stats.h"
#include "store_key.h"

#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <pthread/qos.h>
#include <time.h>
#include <algorithm>
#include <cstdio>
#include <os/log.h>
#include <cstring>
#include "TegraGM20BABI.h"
#include "TegraGM20BCompilerABI.h"

namespace gm20b {
namespace {

constexpr size_t kTunnelBytes = 64;
constexpr size_t kIdentityStackAt = 40;  // SwitchSeedStore.h: the header's identity, whose first word is the stack

uint64_t answeredAt()
{
#if SWITCH_METAL_DIAGNOSTICS
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    return 0;
#endif
}

}  // namespace

Remote::Remote(const KernelTransport &transport, uint64_t stack) : Remote(transportCall(transport), stack)
{}

Remote::Remote(Call kernel, uint64_t stack) : call_(std::move(kernel)), stack_(stack)
{
    capable();
}

bool Remote::capable()
{
    const CompilerMethods known = methods_.load(std::memory_order_acquire);
    if (known != CompilerMethods::Unknown)
        return known == CompilerMethods::Present;
    TegraGM20BAbiCompilerInfoReply info = {};
    const int called = call(TegraGM20BMethodCompilerInfo, nullptr, 0, &info, sizeof info);
    const CompilerMethods methods = compilerMethods(called, info.flags);
    if (methods == CompilerMethods::Present)
        refresh(info);
    if (methods != CompilerMethods::Unknown)
        methods_.store(methods, std::memory_order_release);
    else if (called == kIOReturnBadArgument)
        os_log_error(
            OS_LOG_DEFAULT,
            "SwitchMetal: the kernel's CompilerInfo refused this build's reply size: the GPU kext is of another "
            "build than this process's Metal driver, so no state the pack lacks is compiled");
    else
        os_log_error(OS_LOG_DEFAULT,
                     "SwitchMetal: the kernel's CompilerInfo failed (0x%x): this ask has no shader compiler", called);
    return methods == CompilerMethods::Present;
}

StoreScope Remote::scope()
{
    if (scopeKnown_.load(std::memory_order_acquire))
        return scope_;
    std::lock_guard<gm20b::UnfairMutex> l(scopeLock_);
    if (scopeKnown_.load(std::memory_order_relaxed))
        return scope_;
    TegraGM20BAbiCompilerInfoReply info = {};
    if (call(TegraGM20BMethodCompilerInfo, nullptr, 0, &info, sizeof info) == kIOReturnSuccess && info.scope.kind) {
        scope_.kind = info.scope.kind;
        scope_.identity.assign(reinterpret_cast<const char *>(info.scope.identity),
                               std::min<size_t>(info.scope.bytes, sizeof info.scope.identity));
        scopeKnown_.store(true, std::memory_order_release);
    }
    return scope_;
}

void Remote::refresh(const TegraGM20BAbiCompilerInfoReply &info)
{
    if (info.flags & (TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered))
        expected_.store(true, std::memory_order_release);
    declined_.store(info.flags & TegraGM20BCompilerDeclined, std::memory_order_release);
    if (info.flags & TegraGM20BCompilerLibraries)
        librariesSupported_.store(true, std::memory_order_release);
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
    if (!capable())
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
    if (capable() && call(TegraGM20BMethodCompilerInfo, nullptr, 0, &info, sizeof info) == kIOReturnSuccess)
        refresh(info);
    return declined_.load(std::memory_order_acquire);
}

Remote::~Remote()
{
    std::map<uint64_t, Pending> pending;
    std::vector<Held> held;
    std::array<Port, std::size(kAnswerClasses)> ports{};
    {
        std::unique_lock<gm20b::Mutex> l(asyncLock_);
        closing_ = true;
        if (asyncWaiters_)
            asyncDrained_.notify_all();
        pending.swap(pending_);
        confirmedPending_ = 0;
        held.swap(held_);
        asyncDrained_.wait(l, [&] {
            return asyncInFlight_ == 0;
        });
        ports.swap(ports_);
    }
    heldChanged_.changed();
    for (Port &p : ports) {
        if (p.port)
            IONotificationPortDestroy(static_cast<IONotificationPortRef>(p.port));
        p.port = nullptr;
    }
    for (auto &[cookie, entry] : pending)
        entry.done(Answer::Unavailable, Answered{});
    for (auto &h : held)
        h.done(Answer::Unavailable, Answered{});
    for (Port &p : ports)
        if (p.queue)
            dispatch_release(static_cast<dispatch_queue_t>(p.queue));
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

int Remote::call(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize)
{
    return call_(method, in, inSize, out, outSize, nullptr, 0);
}

uint32_t Remote::portAt(qos_class_t cls)
{
    size_t slot = 0;
    while (slot + 1 < ports_.size() && kAnswerClasses[slot] > cls)
        slot++;
    Port &p = ports_[slot];
    if (!p.port) {
        IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
        if (!port)
            return 0;
        p.queue = dispatch_queue_create(
            "com.brunostjohn.switch.gm20b.remote",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, kAnswerClasses[slot], 0));
        IONotificationPortSetDispatchQueue(port, static_cast<dispatch_queue_t>(p.queue));
        p.port = port;
    }
    return IONotificationPortGetMachPort(static_cast<IONotificationPortRef>(p.port));
}

// The store a compiler registered, checked to be this build's (its pack rules are the compiler's: both load the one
// installed pack)
void Remote::map(uint32_t generation)
{
    std::lock_guard<gm20b::UnfairMutex> l(mapLock_);
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

bool Remote::find(const Digest &name, std::string_view &bytes)
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
    bytes = std::string_view(reinterpret_cast<const char *>(found.data), found.size);
    return true;
}

bool Remote::find(const Digest &name, std::string &bytes)
{
    std::string_view view;
    if (!find(name, view))
        return false;
    bytes.assign(view.data(), view.size());
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

Remote::Answer Remote::compile(const std::string &request, Answered *answered)
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
    if (answered)
        *answered = {reply.how, reply.times, answeredAt()};
    if (reply.result == TegraGM20BCompileUnavailable)
        recheck();
    return answerOf(reply.result);
}

bool Remote::compileAsync(const std::string &request, bool waited, uint32_t qos, Done done, uint64_t *cookieOut)
{
    if (!serving() || !taking())
        return false;
    std::unique_lock<gm20b::Mutex> l(asyncLock_);
    if (closing_)
        return false;
    const uint64_t cookie = nextCookie_++;
    if (!ask(l, {request, waited, qos, std::move(done), cookie}, false))
        return false;
    if (cookieOut)
        *cookieOut = cookie;
    return true;
}

bool Remote::waitAsync(uint64_t cookie)
{
    if (!cookie || !waitSupported_.load(std::memory_order_acquire))
        return false;
    uint64_t skipThrough = 0;
    for (;;) {
        uint64_t active = 0;
        bool isHeld = false;
        bool unconfirmedSelf = false;
        uint64_t snapshotHead = 0;
        bool waitable = false;
        {
            std::lock_guard<gm20b::Mutex> l(asyncLock_);
            if (const auto self = pending_.find(cookie); self != pending_.end()) {
                if (self->second.confirmed)
                    active = cookie;
                else
                    unconfirmedSelf = true;
            } else {
                isHeld = std::any_of(held_.begin(), held_.end(), [cookie](const Held &h) {
                    return h.cookie == cookie;
                });
                if (isHeld) {
                    for (auto it = pending_.upper_bound(skipThrough); it != pending_.end(); ++it)
                        if (it->second.confirmed) {
                            active = it->first;
                            break;
                        }
                    if (!active) {
                        for (const auto &[c, p] : pending_)
                            if (p.confirmed) {
                                snapshotHead = c;
                                break;
                            }
                        waitable = snapshotHead != 0 || asyncInFlight_ > 0 || promoting_;
                    }
                }
            }
        }
        if (active) {
            const TegraGM20BAbiCompileWait in = {active, 0, 0};
            const int r = call(TegraGM20BMethodCompileWait, &in, sizeof in, nullptr, 0);
            if (r == kIOReturnUnsupported) {
                waitSupported_.store(false, std::memory_order_release);
                return false;
            }
            if (active == cookie)
                return r == kIOReturnAborted;
            if (r == kIOReturnNotFound) {
                skipThrough = active;
                continue;
            }
            return r == kIOReturnAborted || r == kIOReturnSuccess;
        }
        if (!unconfirmedSelf && (!isHeld || !waitable))
            return false;
        heldChanged_.waitOnce([&] {
            std::lock_guard<gm20b::Mutex> l(asyncLock_);
            if (const auto self = pending_.find(cookie); self != pending_.end())
                return self->second.confirmed;
            const bool stillHeld = std::any_of(held_.begin(), held_.end(), [cookie](const Held &h) {
                return h.cookie == cookie;
            });
            if (!stillHeld)
                return true;
            uint64_t firstConfirmed = 0;
            for (const auto &[c, p] : pending_) {
                if (!p.confirmed)
                    continue;
                if (!firstConfirmed)
                    firstConfirmed = c;
                if (c > skipThrough)
                    return true;
            }
            if (!firstConfirmed)
                return asyncInFlight_ == 0 && !promoting_;
            return firstConfirmed != snapshotHead;
        });
    }
}

uint64_t Remote::registerLibrary(const void *bytes, size_t size, uint64_t source)
{
    if (!bytes || !size || size > TegraGM20BCompileRequestLimit || !capable() ||
        !librariesSupported_.load(std::memory_order_acquire))
        return 0;
    const RegisteredLibrary key{bytes, size, source};
    std::unique_lock<gm20b::Mutex> l(libraryLock_);
    for (;;) {
        if (!librariesSupported_.load(std::memory_order_acquire))
            return 0;
        if (const auto found = registeredLibraries_.find(key); found != registeredLibraries_.end())
            return found->second;
        if (!registeringLibraries_.count(key)) {
            registeringLibraries_.insert(key);
            break;
        }
        ++libraryWaiters_;
        libraryReady_.wait(l, [&] {
            return !registeringLibraries_.count(key);
        });
        --libraryWaiters_;
    }
    l.unlock();
    const TegraGM20BAbiCompilerRegisterLibrary in = {uint64_t(uintptr_t(bytes)), size, 0, 0};
    TegraGM20BAbiCompilerRegisterLibraryReply reply = {};
    const int called = call(TegraGM20BMethodCompilerRegisterLibrary, &in, sizeof in, &reply, sizeof reply);
    if (called == kIOReturnUnsupported)
        librariesSupported_.store(false, std::memory_order_release);
    l.lock();
    registeringLibraries_.erase(key);
    uint64_t id = 0;
    if (called == kIOReturnSuccess && reply.id) {
        registeredLibraries_.emplace(key, reply.id);
        id = reply.id;
    }
    if (libraryWaiters_)
        libraryReady_.notify_all();
    return id;
}

bool Remote::ask(std::unique_lock<gm20b::Mutex> &l, Held &&held, bool fromHeld)
{
    if (closing_)
        return false;
    const uint64_t cookie = held.cookie ? held.cookie : nextCookie_++;
    held.cookie = cookie;
    const uint32_t port = portAt(held.waited ? answerClass(held.qos) : QOS_CLASS_BACKGROUND);
    if (!port)
        return false;
    const TegraGM20BAbiCompile in = {uint64_t(uintptr_t(held.request.data())),
                                     held.request.size(),
                                     cookie,
                                     stack_,
                                     held.waited ? held.qos : uint32_t(QOS_CLASS_BACKGROUND),
                                     held.waited ? uint32_t(TegraGM20BCompileWaited) : 0u};
    uint64_t reference[kOSAsyncRef64Count] = {};
    reference[kIOAsyncCalloutFuncIndex] = uint64_t(reinterpret_cast<uintptr_t>(&delivered));
    reference[kIOAsyncCalloutRefconIndex] = uint64_t(reinterpret_cast<uintptr_t>(this));

    for (;;) {
        if (closing_)
            return false;
        const uint64_t seenDeliveries = deliveredCount_;
        pending_.emplace(cookie, Pending{held.done, false});
        ++asyncInFlight_;
        l.unlock();
        const int asked = call_(TegraGM20BMethodCompile, &in, sizeof in, nullptr, 0, reference, port);
        if (asked == kIOReturnNotReady)
            recheck();
        l.lock();
        --asyncInFlight_;
        if (asked == kIOReturnSuccess) {
            if (const auto it = pending_.find(cookie); it != pending_.end()) {
                it->second.confirmed = true;
                ++confirmedPending_;
            }
            if ((closing_ && asyncInFlight_ == 0) || asyncWaiters_)
                asyncDrained_.notify_all();
            if (fromHeld)
                heldChanged_.changed();
            return true;
        }
        const bool stillPending = pending_.erase(cookie) > 0;
        if ((closing_ && asyncInFlight_ == 0) || asyncWaiters_)
            asyncDrained_.notify_all();
        if (!stillPending)
            return true;
        if (asked != kIOReturnNoResources || closing_) {
            if (fromHeld)
                heldChanged_.changed();
            return false;
        }
        if (confirmedPending_ == 0 && deliveredCount_ == seenDeliveries && asyncInFlight_ > 0) {
            ++asyncWaiters_;
            asyncDrained_.wait(l, [&] {
                return closing_ || confirmedPending_ > 0 || deliveredCount_ != seenDeliveries || asyncInFlight_ == 0;
            });
            --asyncWaiters_;
        }
        if (closing_) {
            if (fromHeld)
                heldChanged_.changed();
            return false;
        }
        if (confirmedPending_ > 0) {
            if (fromHeld) {
                held_.insert(held_.begin(), std::move(held));
                heldChanged_.changed();
            } else {
                held_.push_back(std::move(held));
            }
            return true;
        }
        if (deliveredCount_ != seenDeliveries)
            continue;
        if (fromHeld)
            heldChanged_.changed();
        return false;
    }
}

void Remote::delivered(void *refcon, int result, void **words, uint32_t count)
{
    auto *remote = static_cast<Remote *>(refcon);
    // IOKit hands the message's own packed words, four bytes past an eight-byte boundary
    uint64_t word[TegraGM20BCompileAnswerWords] = {};
    memcpy(word, reinterpret_cast<const unsigned char *>(words),
           std::min<size_t>(count, TegraGM20BCompileAnswerWords) * sizeof *word);
    const uint64_t packed = word[1];
    const Answered answered = {
        0, {word[2], word[3], word[4], uint32_t(word[5]), uint32_t(word[5] >> 32)}, answeredAt()};
    const int32_t answer = int32_t(uint32_t(packed));
    const bool needRecheck = result == kIOReturnSuccess && answer == TegraGM20BCompileUnavailable;
    Done done;
    std::vector<Done> refused;
    bool runRecheck = false;
    {
        std::unique_lock<gm20b::Mutex> l(remote->asyncLock_);
        const auto found = remote->pending_.find(word[0]);
        if (found == remote->pending_.end())
            return;
        done = std::move(found->second.done);
        if (found->second.confirmed)
            --remote->confirmedPending_;
        remote->pending_.erase(found);
        ++remote->deliveredCount_;
        if (remote->asyncWaiters_)
            remote->asyncDrained_.notify_all();
        if (result == kIOReturnSuccess)
            remote->latest_.store(uint32_t(packed >> 32), std::memory_order_release);
        if (!remote->closing_) {
            if (remote->promoting_) {
                remote->promoteAgain_ = true;
            } else if (!remote->held_.empty()) {
                remote->promoting_ = true;
                do {
                    remote->promoteAgain_ = false;
                    while (!remote->held_.empty() && !remote->closing_) {
                        Held next = std::move(remote->held_.front());
                        remote->held_.erase(remote->held_.begin());
                        Done nextDone = next.done;
                        if (remote->ask(l, std::move(next), true))
                            break;
                        refused.push_back(std::move(nextDone));
                    }
                } while (remote->promoteAgain_ && !remote->held_.empty() && !remote->closing_);
                remote->promoting_ = false;
            }
            if (needRecheck && !remote->closing_) {
                ++remote->asyncInFlight_;
                runRecheck = true;
            }
        }
        remote->heldChanged_.changed();
    }
    if (runRecheck) {
        remote->recheck();
        std::lock_guard<gm20b::Mutex> l(remote->asyncLock_);
        if (--remote->asyncInFlight_ == 0 && remote->closing_)
            remote->asyncDrained_.notify_all();
    }
    for (const auto &r : refused)
        r(Answer::Unavailable, Answered{});
    if (result != kIOReturnSuccess)
        return done(Answer::Unavailable, Answered{});
    done(answerOf(answer), answered);
}

}  // namespace gm20b
