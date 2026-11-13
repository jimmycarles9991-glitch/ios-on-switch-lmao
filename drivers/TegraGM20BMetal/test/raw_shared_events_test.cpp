// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_shared_events.h"
#include "../raw/runtime/raw_batch.h"
#include "../raw/runtime/raw_kernel.h"
#include "TegraGM20BJobABI.h"
#include <IOKit/IOKitLib.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

namespace {
unsigned checks;

void check(bool value, const char *message)
{
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
    ++checks;
}

struct Notification {
    std::array<uint64_t, kOSAsyncRef64Count> reference{};
    uint64_t cookie = 0;
    uint32_t flags = 0;
};

struct Model {
    bool tunnel = false;
    uint32_t selector = 7;
    uint64_t nextHandle = 1;
    int probeResult = kIOReturnNotReady, armResult = 0;
    unsigned ports = 0, destroyed = 0, cancellations = 0;
    std::map<uint64_t, uint32_t> imports;
    std::vector<Notification> notifications;
    std::vector<TegraGM20BAbiSurfaceUse> resourceUses;
    std::vector<uint8_t> submitted;
    unsigned submissions = 0;
    bool pushFences = false;
    std::vector<std::vector<uint8_t>> records;
    unsigned cpuCalls = 0;
    int cpuReturn = 0;
    TegraGM20BAbiCpuComplete cpuRequest{};
    std::vector<TegraGM20BAbiCpuRange> cpuRanges;
} model;

void fire(const Notification &notification, int result)
{
    auto callback = reinterpret_cast<IOAsyncCallback1>(uintptr_t(notification.reference[kIOAsyncCalloutFuncIndex]));
    callback(reinterpret_cast<void *>(uintptr_t(notification.reference[kIOAsyncCalloutRefconIndex])), result,
             reinterpret_cast<void *>(uintptr_t(notification.cookie)));
}

uint32_t decode(uint32_t method, const void *&input, size_t &size)
{
    if (!model.tunnel)
        return method;
    check(method == model.selector && size >= 8, "sandbox selector is preserved");
    uint32_t prefix[2];
    memcpy(prefix, input, sizeof prefix);
    check(prefix[0] == TegraGM20BTunnelMagic, "sandbox tunnel magic");
    input = static_cast<const uint8_t *>(input) + sizeof prefix;
    size -= sizeof prefix;
    return prefix[1];
}

template <class T> const T &request(const void *input, size_t size)
{
    check(size == sizeof(T), "exact event request framing");
    return *static_cast<const T *>(input);
}
}

extern "C" kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t method, const void *input,
                                                   size_t size, void *output, size_t *outputSize)
{
    check(connection == 42, "same raw connection");
    if (model.tunnel) {
        if (!size)
            return kIOReturnUnsupported;
        if (size == 8 && static_cast<const uint32_t *>(input)[0] == TegraGM20BTunnelMagic &&
            static_cast<const uint32_t *>(input)[1] == TegraGM20BTunnelProbe)
            return method == model.selector ? KERN_SUCCESS : kIOReturnUnsupported;
    }
    method = decode(method, input, size);
    switch (method) {
    case TegraGM20BMethodGetInfo: {
        check(!size && outputSize && *outputSize == sizeof(TegraGM20BAbiInfo), "kernel info framing");
        auto &info = *static_cast<TegraGM20BAbiInfo *>(output);
        info = {};
        info.abi_version = TegraGM20BAbiVersion;
        info.flags = TegraGM20BInfoSubmitAddress | (model.pushFences ? uint32_t(TegraGM20BInfoPushFences) : 0u);
        info.va_start = 0x1000000;
        info.va_end = 0x100000000;
        info.page_size = 4096;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodSubmitJob: {
        const auto &address = request<TegraGM20BAbiSubmitAddress>(input, size);
        check(!address.reserved && outputSize && *outputSize == sizeof(TegraGM20BAbiExecReply), "addressed reply ABI");
        const auto *bytes = reinterpret_cast<const uint8_t *>(uintptr_t(address.address));
        model.submitted.assign(bytes, bytes + address.bytes);
        model.records.push_back(model.submitted);
        *static_cast<TegraGM20BAbiExecReply *>(output) = {++model.submissions, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodCpuComplete: {
        const auto &r = request<TegraGM20BAbiCpuComplete>(input, size);
        check(!r.reserved && outputSize && *outputSize == sizeof(TegraGM20BAbiCpuCompleteReply),
              "CPU completion reply ABI");
        model.cpuRequest = r;
        const auto *ranges = reinterpret_cast<const TegraGM20BAbiCpuRange *>(uintptr_t(r.ranges_address));
        model.cpuRanges.assign(ranges, ranges + r.range_count);
        ++model.cpuCalls;
        *static_cast<TegraGM20BAbiCpuCompleteReply *>(output) = {r.range_count, 0, 0, 0, 0, 0};
        return model.cpuReturn;
    }
    case TegraGM20BMethodEventImport: {
        const auto &r = request<TegraGM20BAbiEventImport>(input, size);
        check(!r.reserved && outputSize && *outputSize == sizeof(TegraGM20BAbiEventImportReply), "import reply ABI");
        const uint64_t handle = model.nextHandle++;
        model.imports[handle] = r.port;
        *static_cast<TegraGM20BAbiEventImportReply *>(output) = {handle, uint64_t(1) << 50};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodEventRelease: {
        const auto &r = request<TegraGM20BAbiEventRelease>(input, size);
        check(model.imports.erase(r.handle) == 1, "each imported handle released once");
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodEventObserve: {
        const auto &r = request<TegraGM20BAbiEventObserve>(input, size);
        check(model.imports.count(r.handle) && !r.reserved, "observer owns retained imported handle");
        if (r.flags == TegraGM20BEventObserveProbe) {
            check(r.value > UINT32_MAX, "probe preserves full-width timeline");
            return model.probeResult;
        }
        check(r.flags == TegraGM20BEventObserveCancel && r.cookie, "synchronous one-shot cancel");
        ++model.cancellations;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodEventWord: {
        const auto &r = request<TegraGM20BAbiEventWord>(input, size);
        check(r.handle == 99 && r.channel == 3 && !r.reserved && outputSize &&
                  *outputSize == sizeof(TegraGM20BAbiEventWordReply),
              "event word framing");
        *static_cast<TegraGM20BAbiEventWordReply *>(output) = {0xfffff800, (uint64_t(1) << 48) + 10, 4, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodResourceObserve: {
        const auto &r = request<TegraGM20BAbiResourceObserve>(input, size);
        check(r.cookie && !r.uses_address && !r.use_count && !r.reserved && r.flags == TegraGM20BResourceObserveCancel,
              "resource cancellation uses only its cookie");
        ++model.cancellations;
        return KERN_SUCCESS;
    }
    default:
        std::abort();
    }
}

extern "C" io_service_t IOServiceGetMatchingService(mach_port_t, CFDictionaryRef matching)
{
    CFRelease(matching);
    return 111;
}

extern "C" kern_return_t IOServiceOpen(io_service_t service, task_port_t, uint32_t type, io_connect_t *connection)
{
    check(service == 111 && type == TegraGM20BOpenType, "raw service open contract");
    *connection = 42;
    return KERN_SUCCESS;
}

extern "C" kern_return_t IOObjectRelease(io_object_t)
{
    return KERN_SUCCESS;
}

extern "C" kern_return_t IOServiceClose(io_connect_t)
{
    return KERN_SUCCESS;
}

extern "C" kern_return_t IOConnectCallAsyncStructMethod(mach_port_t connection, uint32_t method, mach_port_t wakePort,
                                                        uint64_t *reference, uint32_t referenceCount, const void *input,
                                                        size_t size, void *, size_t *)
{
    check(connection == 42 && wakePort == 55, "asynchronous call uses admitted connection and port");
    method = decode(method, input, size);
    check(referenceCount == kOSAsyncRef64Count, "async reference ABI");
    if (method == TegraGM20BMethodResourceObserve) {
        const auto &r = request<TegraGM20BAbiResourceObserve>(input, size);
        check(r.cookie && r.uses_address && r.use_count && r.use_count <= TegraGM20BMaxJobUses && !r.flags &&
                  !r.reserved,
              "addressed resource observer framing");
        const auto *uses = reinterpret_cast<const TegraGM20BAbiSurfaceUse *>(uintptr_t(r.uses_address));
        model.resourceUses.assign(uses, uses + r.use_count);
        Notification notification;
        std::copy(reference, reference + referenceCount, notification.reference.begin());
        notification.cookie = r.cookie;
        model.notifications.push_back(notification);
        return model.armResult;
    }
    check(method == TegraGM20BMethodEventObserve, "event observer method");
    const auto &r = request<TegraGM20BAbiEventObserve>(input, size);
    check(model.imports.count(r.handle) && r.cookie && (r.flags == 0 || r.flags == TegraGM20BEventObserveTerminal) &&
              !r.reserved,
          "asynchronous observer framing");
    check(r.value > UINT32_MAX, "observer preserves full-width timeline");
    Notification notification;
    std::copy(reference, reference + referenceCount, notification.reference.begin());
    notification.cookie = r.cookie;
    notification.flags = r.flags;
    model.notifications.push_back(notification);
    return model.armResult;
}

extern "C" IONotificationPortRef IONotificationPortCreate(mach_port_t)
{
    ++model.ports;
    return reinterpret_cast<IONotificationPortRef>(uintptr_t(55));
}

extern "C" mach_port_t IONotificationPortGetMachPort(IONotificationPortRef)
{
    return 55;
}

extern "C" void IONotificationPortSetDispatchQueue(IONotificationPortRef, dispatch_queue_t)
{}

extern "C" void IONotificationPortDestroy(IONotificationPortRef)
{
    ++model.destroyed;
}

static void run(bool tunnel)
{
    model = {};
    model.tunnel = tunnel;
    auto transport = raw::SharedEvents::create(42, tunnel, model.selector);
    auto event = transport->import(101);
    check(event && event->handle() && model.imports.size() == 1, "native event import");
    constexpr uint64_t value = (uint64_t(1) << 48) + 123;
    check(transport->probe(*event, value) == kIOReturnNotReady && model.ports == 0,
          "readiness does not allocate a listener");
    model.probeResult = kIOReturnAborted;
    check(transport->probe(*event, value) == kIOReturnAborted, "failed producer truth preserved");
    unsigned calls = 0;
    int status = -1, result = 0;
    auto watch = transport->observe(
        event, value,
        [&](int code) {
            ++calls;
            status = code;
        },
        result);
    check(watch && !result && model.ports == 1 && !calls, "observer is nonblocking");
    Notification delivered = model.notifications.back();
    check(delivered.flags == 0, "native readiness retains producer-publication observation");
    fire(delivered, kIOReturnAborted);
    fire(delivered, KERN_SUCCESS);
    check(calls == 1 && status == kIOReturnAborted, "duplicate delivery cannot manufacture success");
    watch.reset();
    check(model.cancellations == 1, "delivered watch releases kernel registration");
    auto capture = transport->import(102);
    watch = transport->observe(
        event, value,
        [capture, &calls](int) {
            ++calls;
        },
        result);
    Notification cancelled = model.notifications.back();
    capture.reset();
    watch.reset();
    fire(cancelled, KERN_SUCCESS);
    check(calls == 1 && model.imports.size() == 1, "cancel releases captured resources outside transport lock");
    model.armResult = kIOReturnNoResources;
    check(!transport->observe(
              event, value,
              [&](int) {
                  ++calls;
              },
              result) &&
              result == kIOReturnNoResources,
          "registration refusal is explicit");
    fire(model.notifications.back(), KERN_SUCCESS);
    check(calls == 1, "refused registration cannot call user handler");
    model.armResult = KERN_SUCCESS;
    watch = transport->observe(
        event, value,
        [&](int) {
            ++calls;
            model.probeResult = KERN_SUCCESS;
            check(transport->probe(*event, value) == KERN_SUCCESS, "callback can reenter transport");
        },
        result);
    fire(model.notifications.back(), KERN_SUCCESS);
    check(calls == 2 && model.ports == 1, "notification port reused");
    watch.reset();
    for (unsigned i = 0; i < 64; ++i) {
        std::atomic<unsigned> raced{0};
        watch = transport->observe(
            event, value,
            [&](int) {
                raced.fetch_add(1);
            },
            result);
        const Notification concurrent = model.notifications.back();
        std::thread delivery([concurrent] {
            fire(concurrent, KERN_SUCCESS);
        });
        watch.reset();
        delivery.join();
        check(raced.load() <= 1, "cancel racing delivery invokes callback at most once");
    }
    watch = transport->observe(
        event, value,
        [&](int) {
            ++calls;
        },
        result);
    Notification late = model.notifications.back();
    transport->shutdown();
    check(model.imports.empty() && model.destroyed == 1, "shutdown cancels observers and releases all imports");
    fire(late, KERN_SUCCESS);
    transport.reset();
    fire(late, kIOReturnAborted);
    watch.reset();
    event.reset();
    check(calls == 2 && model.destroyed == 1, "late callbacks cannot access freed state");
}

static void terminal(bool tunnel)
{
    model = {};
    model.tunnel = tunnel;
    auto transport = raw::SharedEvents::create(42, tunnel, model.selector);
    auto event = transport->import(101);
    constexpr uint64_t value = (uint64_t(1) << 48) + 123;
    model.probeResult = KERN_SUCCESS;
    check(transport->probe(*event, value) == KERN_SUCCESS, "producer is already published");
    unsigned wakes = 0;
    int result = 0;
    auto completion = transport->completion(
        event, value,
        [&] {
            ++wakes;
        },
        result);
    check(completion && !result && completion->result() == kIOReturnNotReady && !wakes,
          "published producer is not terminal completion");
    const Notification failed = model.notifications.back();
    check(failed.flags == TegraGM20BEventObserveTerminal, "compatibility requests terminal authority");
    fire(failed, kIOReturnAborted);
    check(wakes == 1 && completion->result() == kIOReturnAborted,
          "producer failure wakes compatibility consumer without an event signal");
    fire(failed, KERN_SUCCESS);
    check(wakes == 1 && completion->result() == kIOReturnAborted, "later success cannot replace retained failure");
    completion.reset();
    completion = transport->completion(
        event, value + 1,
        [&] {
            ++wakes;
        },
        result);
    check(completion && completion->result() == kIOReturnNotReady, "new value has independent completion");
    fire(model.notifications.back(), KERN_SUCCESS);
    check(wakes == 2 && completion->result() == KERN_SUCCESS, "terminal success wakes consumer");
    completion.reset();
    auto capture = std::make_shared<int>(1);
    std::weak_ptr<int> weak = capture;
    completion = transport->completion(
        event, value + 2,
        [capture, &wakes] {
            ++wakes;
        },
        result);
    const Notification cancelled = model.notifications.back();
    capture.reset();
    completion.reset();
    check(weak.expired(), "dropping pending consumer cancels without a retention cycle");
    fire(cancelled, KERN_SUCCESS);
    check(wakes == 2, "cancelled terminal delivery cannot wake released consumer");
    model.armResult = kIOReturnNoResources;
    check(!transport->completion(
              event, value + 3,
              [&] {
                  ++wakes;
              },
              result) &&
              result == kIOReturnNoResources,
          "terminal registration failure is explicit");
    fire(model.notifications.back(), KERN_SUCCESS);
    check(wakes == 2, "failed registration cannot wake consumer");
}

static bool submitJob(raw::Kernel &kernel, const TegraGM20BAbiPush *pushes, uint32_t count,
                      const raw::SurfaceList &uses, uint32_t &sequence, int *result, raw::JobVisibility visibility,
                      const raw::JobEvents &events = {})
{
    raw::KernelJob job;
    job.channel = 3;
    job.pushes = pushes;
    job.pushCount = count;
    job.uses = uses.uses().data();
    job.useCount = uint32_t(uses.uses().size());
    job.visibility = visibility;
    job.events = events;
    return kernel.submitJob(job, sequence, result);
}

static void submit(bool tunnel)
{
    model = {};
    model.tunnel = tunnel;
    raw::Kernel kernel;
    std::string error;
    check(kernel.open(error), "actual Kernel opens test connection");
    raw::SurfaceList resources;
    resources.add(9, true);
    const TegraGM20BAbiPush push{0x10000, 24, 0};
    const TegraGM20BAbiEventUse wait{99, (uint64_t(1) << 48) + 19};
    const TegraGM20BAbiEventUse signal{100, (uint64_t(1) << 49) + 23};
    const raw::JobEvents events{&wait, &signal, 1, 1};
    uint32_t sequence = 0;
    int result = 0;
    const bool accepted =
        submitJob(kernel, &push, 1, resources, sequence, &result, raw::JobVisibility::Internal, events);
    check(accepted && sequence == 1 && model.submissions == 1, "pushes and events publish in one call");
    const auto &header = *reinterpret_cast<const TegraGM20BAbiJob *>(model.submitted.data());
    check(header.channel == 3 && header.push_count == 1 && header.shared_count == 1 && !header.set &&
              !header.set_generation && header.flags == TegraGM20BJobHasEvents,
          "event and visibility flags coexist");
    const size_t offset = sizeof header + sizeof push + sizeof(TegraGM20BAbiSurfaceUse);
    const auto &trailer = *reinterpret_cast<const TegraGM20BAbiSubmitEvents *>(model.submitted.data() + offset);
    check(model.submitted.size() == offset + sizeof trailer + 2 * sizeof wait && trailer.wait_count == 1 &&
              trailer.signal_count == 1 && !trailer.reserved,
          "event trailer follows exact resource uses");
    check(!memcmp(model.submitted.data() + offset + sizeof trailer, &wait, sizeof wait) &&
              !memcmp(model.submitted.data() + offset + sizeof trailer + sizeof wait, &signal, sizeof signal),
          "full-width waits precede full-width signals");
    raw::SurfaceList empty;
    check(submitJob(kernel, nullptr, 0, empty, sequence, &result, raw::JobVisibility::External,
                    {nullptr, &signal, 0, 1}) &&
              sequence == 2 &&
              reinterpret_cast<const TegraGM20BAbiJob *>(model.submitted.data())->flags ==
                  (TegraGM20BJobExternal | TegraGM20BJobHasEvents),
          "zero-push event-only job is atomic");
    check(!submitJob(kernel, &push, 1, resources, sequence, &result, raw::JobVisibility::Internal,
                     {&wait, &signal, 32, 1}) &&
              model.submissions == 2,
          "oversized event list cannot publish");
    check(!submitJob(kernel, &push, 1, resources, sequence, &result, raw::JobVisibility::Internal,
                     {nullptr, nullptr, 1, 0}) &&
              model.submissions == 2,
          "missing event array cannot publish");
}

// A command buffer's work batches, submitted as CommandBuffer::commit submits them: one SubmitJob carrying every
// push and the use list once, its batch ends marked as internal fences, or, from a kernel without them, one per batch.
static void batchSubmission(bool tunnel, bool supported)
{
    model = {};
    model.tunnel = tunnel;
    model.pushFences = supported;
    raw::Kernel kernel;
    std::string error;
    check(kernel.open(error), "actual Kernel opens test connection");
    check(kernel.maxFences() == (supported ? uint32_t(TegraGM20BMaxJobFences) : 0u), "fence capacity follows the bit");
    raw::SurfaceList resources;
    resources.add(9, true);
    resources.add(10, false);
    raw::PushBatches plan;
    for (uint32_t batch = 0; batch < 4; ++batch) {
        plan.append({0x10000 + batch * 0x1000, 24, 0}, false);
        plan.append({0x10800 + batch * 0x1000, 8, 0}, false);
        plan.finishBatch();
    }
    uint32_t sequence = 0;
    const bool sent = plan.submit(kernel.maxPushes(), kernel.maxFences(), [&](size_t at, size_t n) {
        return submitJob(kernel, plan.data() + at, uint32_t(n), resources, sequence, nullptr,
                         raw::JobVisibility::Internal);
    });
    check(sent, "every batch submitted");
    check(model.submissions == (supported ? 1u : 4u), "one kernel call per command buffer with internal fences");
    unsigned pushes = 0, fences = 0;
    for (const auto &record : model.records) {
        const auto &header = *reinterpret_cast<const TegraGM20BAbiJob *>(record.data());
        check(header.shared_count == 2, "each call carries the use list once");
        const auto *submitted = reinterpret_cast<const TegraGM20BAbiPush *>(record.data() + sizeof header);
        for (uint32_t i = 0; i < header.push_count; ++i) {
            const bool fenced = submitted[i].flags & TegraGM20BPushFence;
            check(fenced == (i % 2 == 1 && i + 1 < header.push_count), "a fence at each batch end inside the job");
            fences += fenced;
        }
        pushes += header.push_count;
    }
    check(pushes == 8 && fences == (supported ? 3u : 0u), "every push once, every batch end but the last fenced");
    if (supported)
        return;
    TegraGM20BAbiPush fenced[2] = {{0x10000, 24, TegraGM20BPushFence}, {0x10800, 8, 0}};
    int result = 0;
    check(!submitJob(kernel, fenced, 2, resources, sequence, &result, raw::JobVisibility::Internal) &&
              result == kIOReturnBadArgument && model.submissions == 4,
          "a kernel without fences is never sent one");
}

static void rangeSubmission(bool tunnel)
{
    model = {};
    model.tunnel = tunnel;
    raw::Kernel kernel;
    std::string error;
    check(kernel.open(error), "range connection opens");
    raw::SurfaceList resources;
    resources.addAddress(0x400000, 4096, true, TegraGM20BSurfaceRange);
    resources.addAddress(0x408000, 8192, true, TegraGM20BSurfaceRange);
    resources.addAddress(0x500000, 256, false, TegraGM20BSurfaceRange | TegraGM20BSurfaceHostWritten);
    const TegraGM20BAbiPush push{0x10000, 24, 0};
    uint32_t sequence = 0;
    int result = 0;
    const bool accepted = submitJob(kernel, &push, 1, resources, sequence, &result, raw::JobVisibility::Internal);
    const size_t offset = sizeof(TegraGM20BAbiJob) + sizeof push;
    check(accepted && !result && sequence == 1 && model.submissions == 1 &&
              model.submitted.size() == offset + 3 * sizeof(TegraGM20BAbiSurfaceUse),
          "exact byte ranges and pushes publish atomically");
    check(!memcmp(model.submitted.data() + offset, resources.uses().data(), 3 * sizeof(TegraGM20BAbiSurfaceUse)),
          "disjoint write extents and host ownership metadata cross actual transport unchanged");
}

static void resourceReadiness(bool tunnel)
{
    model = {};
    model.tunnel = tunnel;
    auto transport = raw::SharedEvents::create(42, tunnel, model.selector);
    std::vector<TegraGM20BAbiSurfaceUse> uses;
    for (uint32_t i = 0; i < TegraGM20BMaxJobUses; ++i)
        uses.push_back({uint64_t(i) + 1, i & 1 ? TegraGM20BSurfaceRead : TegraGM20BSurfaceWrite, 0});
    int result = 0;
    unsigned wakes = 0;
    auto completion = transport->resources(
        uses,
        [&] {
            ++wakes;
        },
        result);
    check(completion && !result && !wakes && completion->result() == kIOReturnNotReady,
          "resource observer never waits for CPU admission");
    check(model.resourceUses.size() == uses.size() &&
              !memcmp(uses.data(), model.resourceUses.data(), uses.size() * sizeof uses[0]),
          "all 1024 resource identities and access intents cross fixed-size transport");
    Notification ready = model.notifications.back();
    fire(ready, KERN_SUCCESS);
    fire(ready, kIOReturnAborted);
    check(wakes == 1 && completion->result() == KERN_SUCCESS, "one-shot resource readiness retains its result");
    completion.reset();
    auto event = transport->import(12);
    completion = transport->resources(
        uses,
        [event, &wakes] {
            ++wakes;
        },
        result);
    Notification cancelled = model.notifications.back();
    event.reset();
    completion.reset();
    check(model.imports.empty(), "resource cancellation releases captures outside transport lock");
    fire(cancelled, KERN_SUCCESS);
    check(wakes == 1, "cancelled resource delivery cannot wake a released queue");
    auto capture = transport->import(13);
    model.armResult = kIOReturnNoResources;
    check(!transport->resources(
              uses, [held = std::move(capture)] {}, result) &&
              result == kIOReturnNoResources,
          "resource registration failure is explicit");
    check(model.imports.empty(), "failed registration destroys reentrant capture outside lock");
    fire(model.notifications.back(), KERN_SUCCESS);
    check(wakes == 1, "failed resource registration never delivers");
    model.armResult = KERN_SUCCESS;
    check(!transport->resources(
              {}, [] {}, result) &&
              result == kIOReturnBadArgument,
          "empty resource observation refused");
    uses.push_back({0xffff, TegraGM20BSurfaceRead, 0});
    check(!transport->resources(
              uses, [] {}, result) &&
              result == kIOReturnBadArgument,
          "oversized resource observation refused");
    uses.pop_back();
    completion = transport->resources(
        uses,
        [&] {
            ++wakes;
        },
        result);
    fire(model.notifications.back(), kIOReturnAborted);
    check(wakes == 2 && completion->result() == kIOReturnAborted, "connection/resource failure reaches queue");
    completion.reset();
    completion = transport->resources(
        uses,
        [&] {
            ++wakes;
        },
        result);
    Notification late = model.notifications.back();
    transport->shutdown();
    transport.reset();
    fire(late, KERN_SUCCESS);
    completion.reset();
    check(wakes == 2 && model.destroyed == 1, "late resource notification cannot access closed transport");
}

// A job's signals released mid-job: one SignalRelease per signal after them, Released only on those its pushes release
static void releases(bool tunnel)
{
    model = {};
    model.tunnel = tunnel;
    raw::Kernel kernel;
    std::string error;
    check(kernel.open(error), "release Kernel opens");
    uint64_t address = 0, base = 0;
    uint32_t floor = 0;
    check(kernel.eventWord(99, 3, address, base, floor) == KERN_SUCCESS && address == 0xfffff800 &&
              base == (uint64_t(1) << 48) + 10 && floor == 4,
          "event word reply");
    raw::SurfaceList resources;
    resources.add(9, false);
    const TegraGM20BAbiPush push{0x10000, 24, 0};
    const TegraGM20BAbiEventUse signals[2] = {{99, base + 5}, {98, 7}};
    const uint32_t payloads[2] = {5, 0};
    uint32_t sequence = 0;
    int result = 0;
    check(submitJob(kernel, &push, 1, resources, sequence, &result, raw::JobVisibility::Internal,
                    {nullptr, signals, 0, 2, payloads}),
          "released signals submit");
    const auto &header = *reinterpret_cast<const TegraGM20BAbiJob *>(model.submitted.data());
    const size_t offset = sizeof header + sizeof push + sizeof(TegraGM20BAbiSurfaceUse) +
                          sizeof(TegraGM20BAbiSubmitEvents) + sizeof signals;
    check((header.flags & TegraGM20BJobEventReleases) && (header.flags & TegraGM20BJobHasEvents) &&
              model.submitted.size() == offset + 2 * sizeof(TegraGM20BAbiSignalRelease),
          "release records follow the signals");
    const auto *records = reinterpret_cast<const TegraGM20BAbiSignalRelease *>(model.submitted.data() + offset);
    check(records[0].payload == 5 && records[0].flags == TegraGM20BSignalReleased && !records[1].payload &&
              !records[1].flags,
          "only the released signal is marked");
    const uint32_t none[2] = {0, 0};
    check(
        submitJob(kernel, &push, 1, resources, sequence, &result, raw::JobVisibility::Internal,
                  {nullptr, signals, 0, 2, none}) &&
            !(reinterpret_cast<const TegraGM20BAbiJob *>(model.submitted.data())->flags & TegraGM20BJobEventReleases) &&
            model.submitted.size() == offset,
        "a job releasing nothing carries no records");
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    check(kernel.jobsSubmitted() == 2 && kernel.signalsReleased() == 1, "jobs and released signals counted");
#endif
}

static void cpuCompletion(bool tunnel)
{
    model = {};
    model.tunnel = tunnel;
    raw::Kernel kernel;
    std::string error;
    check(kernel.open(error), "actual CPU completion connection opens");
    raw::CpuCompletion batch;
    batch.channelUid = (uint64_t(9) << 48) | 7;
    batch.channel = 3;
    batch.seqno = 0;
    batch.ranges = {{19, 17, 128}, {20, 0, 64}};
    const auto result = kernel.completeCpu(batch);
    check(result == raw::CpuCompletionResult::Current && model.cpuCalls == 1,
          "cached ranges publish through one completion call");
    check(model.cpuRequest.channel_uid == batch.channelUid && model.cpuRequest.channel == 3 &&
              model.cpuRequest.seqno == 0 && model.cpuRequest.range_count == 2,
          "completion preserves full channel identity and wrapped-zero fence");
    check(model.cpuRanges.size() == 2 && model.cpuRanges[0].handle == 19 && model.cpuRanges[0].offset == 17 &&
              model.cpuRanges[0].bytes == 128,
          "completion preserves exact cached BO subranges");
    model.cpuReturn = kIOReturnNotReady;
    check(kernel.completeCpu(batch) == raw::CpuCompletionResult::Failed && model.cpuCalls == 2,
          "refused exact prefix never falls back to unsafe userspace maintenance");
    batch.ranges.clear();
    check(kernel.completeCpu(batch) == raw::CpuCompletionResult::Current && model.cpuCalls == 2,
          "GPU-only completion emits no kernel call");
}

int main()
{
    run(false);
    run(true);
    terminal(false);
    terminal(true);
    resourceReadiness(false);
    resourceReadiness(true);
    submit(false);
    submit(true);
    batchSubmission(false, true);
    batchSubmission(true, true);
    batchSubmission(false, false);
    rangeSubmission(false);
    rangeSubmission(true);
    releases(false);
    releases(true);
    cpuCompletion(false);
    cpuCompletion(true);
    printf("PASS: %u shared-event transport checks\n", checks);
}
