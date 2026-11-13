// SPDX-License-Identifier: MIT
#include "raw_lock.h"
#include "raw_shared_events.h"
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace raw {
namespace {
raw::Mutex registryLock;
std::map<uintptr_t, std::weak_ptr<SharedEvents>> registry;
std::atomic<uintptr_t> nextIdentity{1};
}

struct SharedEvents::State {
    State(uint32_t conn, bool tunneled, uint32_t sel) : connection(conn), selector(sel), tunnel(tunneled)
    {}

    uint32_t connection, selector;
    bool tunnel;
    std::atomic<bool> closed{false};
    uintptr_t identity = 0;
    IONotificationPortRef port = nullptr;
    dispatch_queue_t queue = nullptr;
    raw::Mutex lock;
    std::set<uint64_t> imports;

    struct Observer {
        uint64_t handle;
        std::function<void(int)> ready;
    };

    std::map<uint64_t, Observer> observers;
    uint64_t nextCookie = 1;

    bool preparePort()
    {
        if (port)
            return true;
        port = IONotificationPortCreate(kIOMainPortDefault);
        if (!port)
            return false;
        queue = dispatch_queue_create("com.brunostjohn.switch.raw.events", DISPATCH_QUEUE_SERIAL);
        IONotificationPortSetDispatchQueue(port, queue);
        return true;
    }

    int call(uint32_t method, const void *input, size_t bytes, void *output = nullptr, size_t outputBytes = 0,
             uint64_t *reference = nullptr)
    {
        if (closed)
            return kIOReturnNotOpen;
        alignas(uint64_t) uint8_t tunneled[sizeof(TegraGM20BAbiEventObserve) + 8];
        if (tunnel) {
            if (bytes > sizeof tunneled - 8)
                return kIOReturnBadArgument;
            const uint32_t prefix[] = {TegraGM20BTunnelMagic, method};
            memcpy(tunneled, prefix, sizeof prefix);
            memcpy(tunneled + sizeof prefix, input, bytes);
            input = tunneled;
            bytes += sizeof prefix;
            method = selector;
        }
        if (reference)
            return IOConnectCallAsyncStructMethod(connection, method, IONotificationPortGetMachPort(port), reference,
                                                  kOSAsyncRef64Count, input, bytes, output,
                                                  outputBytes ? &outputBytes : nullptr);
        return IOConnectCallStructMethod(connection, method, input, bytes, output,
                                         outputBytes ? &outputBytes : nullptr);
    }

    void cancel(uint64_t handle, uint64_t cookie)
    {
        if (handle) {
            const TegraGM20BAbiEventObserve request{handle, 0, cookie, TegraGM20BEventObserveCancel, 0};
            (void)call(TegraGM20BMethodEventObserve, &request, sizeof request);
        } else {
            const TegraGM20BAbiResourceObserve request{cookie, 0, 0, TegraGM20BResourceObserveCancel, 0};
            (void)call(TegraGM20BMethodResourceObserve, &request, sizeof request);
        }
    }
};

SharedEvents::SharedEvents(uint32_t connection, bool tunnel, uint32_t selector)
    : state_(new State(connection, tunnel, selector))
{}

std::shared_ptr<SharedEvents> SharedEvents::create(uint32_t connection, bool tunnel, uint32_t selector)
{
    auto events = std::shared_ptr<SharedEvents>(new SharedEvents(connection, tunnel, selector));
    auto &state = *events->state_;
    state.identity = nextIdentity.fetch_add(1, std::memory_order_relaxed);
    if (!state.identity)
        return nullptr;
    {
        std::lock_guard<raw::Mutex> lock(registryLock);
        registry.emplace(state.identity, events);
    }
    return events;
}

SharedEvents::~SharedEvents()
{
    shutdown();
}

std::shared_ptr<SharedEventLease> SharedEvents::import(uint32_t port)
{
    auto &s = *state_;
    std::lock_guard<raw::Mutex> lock(s.lock);
    const TegraGM20BAbiEventImport request{port, 0};
    TegraGM20BAbiEventImportReply reply{};
    if (s.call(TegraGM20BMethodEventImport, &request, sizeof request, &reply, sizeof reply) || !reply.handle)
        return nullptr;
    s.imports.insert(reply.handle);
    return std::shared_ptr<SharedEventLease>(new SharedEventLease(shared_from_this(), reply.handle));
}

int SharedEvents::probe(const SharedEventLease &event, uint64_t value)
{
    auto &s = *state_;
    if (event.owner().get() != this)
        return kIOReturnBadArgument;
    const TegraGM20BAbiEventObserve request{event.handle(), value, 0, TegraGM20BEventObserveProbe, 0};
    return s.call(TegraGM20BMethodEventObserve, &request, sizeof request);
}

std::shared_ptr<SharedEventWatch> SharedEvents::observe(std::shared_ptr<SharedEventLease> event, uint64_t value,
                                                        std::function<void(int)> ready, int &result, bool terminal)
{
    auto &s = *state_;
    std::function<void(int)> failedReady;
    std::lock_guard<raw::Mutex> lock(s.lock);
    result = kIOReturnBadArgument;
    if (!event || event->owner().get() != this || !s.imports.count(event->handle()) || !ready || s.closed)
        return nullptr;
    if (!s.preparePort()) {
        result = kIOReturnNoResources;
        return nullptr;
    }
    const uint64_t cookie = s.nextCookie;
    if (!cookie) {
        result = kIOReturnNoResources;
        return nullptr;
    }
    ++s.nextCookie;
    s.observers.emplace(cookie, State::Observer{event->handle(), std::move(ready)});
    uint64_t reference[kOSAsyncRef64Count]{};
    reference[kIOAsyncCalloutFuncIndex] = uint64_t(reinterpret_cast<uintptr_t>(&notification));
    reference[kIOAsyncCalloutRefconIndex] = s.identity;
    const TegraGM20BAbiEventObserve request{event->handle(), value, cookie,
                                            terminal ? uint32_t(TegraGM20BEventObserveTerminal) : 0u, 0};
    result = s.call(TegraGM20BMethodEventObserve, &request, sizeof request, nullptr, 0, reference);
    if (result) {
        failedReady = std::move(s.observers.at(cookie).ready);
        s.observers.erase(cookie);
        return nullptr;
    }
    return std::shared_ptr<SharedEventWatch>(new SharedEventWatch(std::move(event), cookie));
}

std::shared_ptr<SharedEventCompletion> SharedEvents::completion(std::shared_ptr<SharedEventLease> event, uint64_t value,
                                                                std::function<void()> ready, int &result)
{
    result = kIOReturnBadArgument;
    if (!ready)
        return nullptr;
    auto completion = std::shared_ptr<SharedEventCompletion>(
        new SharedEventCompletion(std::make_shared<std::atomic<int>>(kIOReturnNotReady)));
    completion->watch_ = observe(
        std::move(event), value,
        [status = completion->result_, ready = std::move(ready)](int code) {
            status->store(code, std::memory_order_release);
            ready();
        },
        result, true);
    return completion->watch_ ? completion : nullptr;
}

std::shared_ptr<ResourceReadyCompletion> SharedEvents::resources(const std::vector<TegraGM20BAbiSurfaceUse> &uses,
                                                                 std::function<void()> ready, int &result)
{
    auto &s = *state_;
    std::function<void(int)> failedReady;
    std::lock_guard<raw::Mutex> lock(s.lock);
    result = kIOReturnBadArgument;
    if (!ready || uses.empty() || uses.size() > TegraGM20BMaxJobUses || s.closed)
        return nullptr;
    if (!s.preparePort()) {
        result = kIOReturnNoResources;
        return nullptr;
    }
    const uint64_t cookie = s.nextCookie;
    if (!cookie) {
        result = kIOReturnNoResources;
        return nullptr;
    }
    ++s.nextCookie;
    auto completion = std::shared_ptr<ResourceReadyCompletion>(
        new ResourceReadyCompletion(std::make_shared<std::atomic<int>>(kIOReturnNotReady)));
    s.observers.emplace(cookie, State::Observer{0, [status = completion->result_, ready = std::move(ready)](int code) {
                                                    status->store(code, std::memory_order_release);
                                                    ready();
                                                }});
    uint64_t reference[kOSAsyncRef64Count]{};
    reference[kIOAsyncCalloutFuncIndex] = uint64_t(reinterpret_cast<uintptr_t>(&notification));
    reference[kIOAsyncCalloutRefconIndex] = s.identity;
    const TegraGM20BAbiResourceObserve request{cookie, uint64_t(reinterpret_cast<uintptr_t>(uses.data())),
                                               uint32_t(uses.size()), 0, 0};
    result = s.call(TegraGM20BMethodResourceObserve, &request, sizeof request, nullptr, 0, reference);
    if (result) {
        failedReady = std::move(s.observers.at(cookie).ready);
        s.observers.erase(cookie);
        return nullptr;
    }
    completion->watch_ = std::shared_ptr<SharedEventWatch>(new SharedEventWatch(shared_from_this(), cookie));
    return completion;
}

void SharedEvents::notification(void *context, int result, void *cookie)
{
    std::shared_ptr<SharedEvents> owner;
    {
        std::lock_guard<raw::Mutex> lock(registryLock);
        const auto found = registry.find(reinterpret_cast<uintptr_t>(context));
        if (found != registry.end())
            owner = found->second.lock();
    }
    if (owner)
        owner->delivered(reinterpret_cast<uintptr_t>(cookie), result);
}

void SharedEvents::delivered(uint64_t cookie, int result)
{
    std::function<void(int)> ready;
    {
        std::lock_guard<raw::Mutex> lock(state_->lock);
        const auto found = state_->observers.find(cookie);
        if (found == state_->observers.end())
            return;
        ready = std::move(found->second.ready);
        state_->observers.erase(found);
    }
    ready(result);
}

void SharedEvents::cancel(uint64_t handle, uint64_t cookie)
{
    std::function<void(int)> cancelled;
    std::lock_guard<raw::Mutex> lock(state_->lock);
    if (auto found = state_->observers.find(cookie); found != state_->observers.end()) {
        cancelled = std::move(found->second.ready);
        state_->observers.erase(found);
    }
    state_->cancel(handle, cookie);
}

void SharedEvents::release(uint64_t handle)
{
    std::lock_guard<raw::Mutex> lock(state_->lock);
    if (!state_->imports.erase(handle))
        return;
    const TegraGM20BAbiEventRelease request{handle};
    (void)state_->call(TegraGM20BMethodEventRelease, &request, sizeof request);
}

void SharedEvents::shutdown()
{
    {
        std::lock_guard<raw::Mutex> lock(registryLock);
        registry.erase(state_->identity);
    }
    auto &s = *state_;
    IONotificationPortRef port;
    dispatch_queue_t queue;
    std::map<uint64_t, State::Observer> cancelled;
    {
        std::lock_guard<raw::Mutex> lock(s.lock);
        if (s.closed)
            return;
        for (const auto &entry : s.observers)
            s.cancel(entry.second.handle, entry.first);
        cancelled.swap(s.observers);
        for (uint64_t handle : s.imports) {
            const TegraGM20BAbiEventRelease request{handle};
            (void)s.call(TegraGM20BMethodEventRelease, &request, sizeof request);
        }
        s.imports.clear();
        s.closed = true;
        port = s.port;
        queue = s.queue;
        s.port = nullptr;
        s.queue = nullptr;
    }
    // Late Mach deliveries contain an identity, never a pointer to released state.
    if (port)
        IONotificationPortDestroy(port);
    if (queue)
        dispatch_release(queue);
}

SharedEventLease::~SharedEventLease()
{
    if (auto owner = owner_.lock())
        owner->release(handle_);
}

SharedEventWatch::~SharedEventWatch()
{
    if (auto owner = event_ ? event_->owner() : owner_.lock())
        owner->cancel(event_ ? event_->handle() : 0, cookie_);
}
}
