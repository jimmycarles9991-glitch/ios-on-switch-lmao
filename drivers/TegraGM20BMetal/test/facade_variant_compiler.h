// SPDX-License-Identifier: MIT
// The shader compiler as the kernel model runs it for a render state's variants (TegraGM20BMetalCubic.mm), and what the
// facade's variant tests read back: a variant's settled state, the programs a job's draws took
#pragma once
#include "facade_harness.h"
#include "../TegraGM20BMetalCompileRequest.h"
#include "../compile_request.h"
#include "../mme.h"
#include "../render_key.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include <IOKit/OSMessageNotification.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

extern "C" {
struct record_count {
    uint64_t sends, locks, mallocs, frees, weaks, retains, dispatches;
};

void record_count_start(void);
void record_count_stop(struct record_count *out);
}

namespace harness {

// Tracks in-flight async compiles for tests that set kernel_model::model.asyncCompile = true: runs each async compile
// off the caller's thread without holding kernel_model::callLock, and services kMethodCompileWait (0x534b).
struct AsyncCompileTracker {
    std::mutex lock;
    std::condition_variable cv;
    std::set<uint64_t> active;
    std::vector<uint64_t> waited;
    std::vector<std::thread> threads;

    AsyncCompileTracker()
    {
        instance() = this;
        kernel_model::model.asyncCompile = true;
        kernel_model::model.compileWait = [this](uint64_t cookie) -> int {
            std::unique_lock<std::mutex> l(lock);
            if (!active.count(cookie))
                return kIOReturnNotFound;
            waited.push_back(cookie);
            cv.notify_all();
            cv.wait(l, [&] {
                return !active.count(cookie);
            });
            return KERN_SUCCESS;
        };
    }

    ~AsyncCompileTracker()
    {
        std::vector<std::thread> join;
        {
            std::lock_guard<std::mutex> l(lock);
            join.swap(threads);
        }
        for (std::thread &t : join)
            if (t.joinable())
                t.join();
        {
            std::lock_guard<std::mutex> l(kernel_model::callLock);
            kernel_model::model.asyncCompile = false;
            kernel_model::model.compileWait = nullptr;
        }
        instance() = nullptr;
    }

    bool waitForCompileWait(size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> l(lock);
        return cv.wait_for(l, timeout, [&] {
            return waited.size() >= count;
        });
    }

    size_t waitCount()
    {
        std::lock_guard<std::mutex> l(lock);
        return waited.size();
    }

    static AsyncCompileTracker *&instance()
    {
        static AsyncCompileTracker *current = nullptr;
        return current;
    }

    static kern_return_t sendReply(mach_port_t port, const uint64_t *reference, uint64_t cookie, int32_t answer,
                                   uint32_t generation)
    {
        const kern_return_t result = KERN_SUCCESS;
        const uint64_t packed = uint64_t(uint32_t(answer)) | uint64_t(generation) << 32;
        const uint64_t words[TegraGM20BCompileAnswerWords] = {cookie, packed};
        constexpr size_t kBytes =
            sizeof(mach_msg_header_t) + sizeof(OSNotificationHeader64) + sizeof result + sizeof words;
        alignas(8) uint8_t bytes[kBytes] = {};
        auto *header = reinterpret_cast<mach_msg_header_t *>(bytes);
        auto *notification = reinterpret_cast<OSNotificationHeader64 *>(bytes + sizeof *header);
        uint8_t *content = bytes + sizeof *header + sizeof *notification;
        header->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
        header->msgh_size = sizeof bytes;
        header->msgh_remote_port = port;
        header->msgh_id = kOSNotificationMessageID;
        notification->size = sizeof result + sizeof words;
        notification->type = kIOAsyncCompletionNotificationType;
        memcpy(notification->reference, reference, sizeof notification->reference);
        memcpy(content, &result, sizeof result);
        memcpy(content + sizeof result, words, sizeof words);
        return mach_msg(header, MACH_SEND_MSG, sizeof bytes, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
};

}  // namespace harness

// One test binary includes this once: the asynchronous compile answered at once, as the kernel's notification (or on a
// worker thread when kernel_model::model.asyncCompile is set)
extern "C" kern_return_t IOConnectCallAsyncStructMethod(mach_port_t connection, uint32_t method, mach_port_t port,
                                                        uint64_t *reference, uint32_t referenceCount, const void *input,
                                                        size_t inputBytes, void *, size_t *)
{
    if (method != TegraGM20BMethodCompile || inputBytes != sizeof(TegraGM20BAbiCompile) ||
        referenceCount != kOSAsyncRef64Count)
        return kIOReturnUnsupported;
    const auto &request = *static_cast<const TegraGM20BAbiCompile *>(input);
    if (harness::AsyncCompileTracker *tracker = harness::AsyncCompileTracker::instance()) {
        std::string payload(reinterpret_cast<const char *>(uintptr_t(request.address)), size_t(request.bytes));
        std::array<uint64_t, kOSAsyncRef64Count> ref{};
        memcpy(ref.data(), reference, sizeof ref);
        std::function<int32_t(const std::string &)> fn;
        int32_t fallback = TegraGM20BCompileUnavailable;
        uint32_t fallbackGen = 1;
        {
            std::lock_guard<std::mutex> l(kernel_model::callLock);
            kernel_model::Model &m = kernel_model::connected(connection);
            m.compileCalls.push_back({request.qos, request.flags});
            m.compileRequests.push_back(payload);
            const size_t at = std::min<size_t>(m.compiles++, m.compileResults.size() - 1);
            fn = m.compiler;
            fallback = m.compileResults[at];
            fallbackGen = m.storeGeneration;
        }
        {
            std::lock_guard<std::mutex> l(tracker->lock);
            tracker->active.insert(request.cookie);
            tracker->threads.emplace_back([tracker, connection, port, ref, cookie = request.cookie,
                                           payload = std::move(payload), fn = std::move(fn), fallback, fallbackGen] {
                int32_t answer = fallback;
                uint32_t generation = fallbackGen;
                if (fn) {
                    answer = fn(payload);
                    std::lock_guard<std::mutex> l(kernel_model::callLock);
                    generation = ++kernel_model::connected(connection).storeGeneration;
                }
                {
                    std::lock_guard<std::mutex> l(tracker->lock);
                    tracker->active.erase(cookie);
                }
                tracker->cv.notify_all();
                harness::AsyncCompileTracker::sendReply(port, ref.data(), cookie, answer, generation);
            });
        }
        return KERN_SUCCESS;
    }
    TegraGM20BAbiCompileReply reply = {};
    size_t replyBytes = sizeof reply;
    const kern_return_t result = IOConnectCallStructMethod(connection, method, input, inputBytes, &reply, &replyBytes);
    if (result != KERN_SUCCESS)
        return result;
    return harness::AsyncCompileTracker::sendReply(port, reference, request.cookie, reply.result, reply.generation);
}

namespace harness {

// Each render request made into a record whose programs carry its forms, kept in the store; `refuses`: its refusal
// stored instead; `fails`: nothing stored and the compile answered as one that could not run; `shapes`: the record
// changed as the test's compiler makes it before it is stored
struct RenderCompiler {
    switch_seed_pack::StoreWriter store;
    std::mutex mutex;
    std::condition_variable answered;
    std::vector<gm20b::CompileRequest> requests;
    std::function<bool(const gm20b::CompileRequest &)> refuses, fails;
    std::function<void(const gm20b::CompileRequest &, gm20b::RuntimeRecord &)> shapes;

    bool serve(const std::string &directory)
    {
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        identity[8] = 0xc3;
        std::string why;
        if (!store.open(directory + "/store", identity, 128, 1u << 22, why))
            return false;
        kernel_model::model.compilerFlags = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected |
                                            TegraGM20BCompilerRegistered | TegraGM20BCompilerStore;
        kernel_model::model.store = store.base();
        kernel_model::model.storeBytes = store.mappedBytes();
        kernel_model::model.compiler = [this](const std::string &bytes) {
            gm20b::CompileRequest request;
            if (!gm20b::decodeRequest(bytes, request) || request.kind != gm20b::RequestKind::Render ||
                request.stages.size() < 2)
                return int32_t(TegraGM20BCompileUnavailable);
            if (fails && fails(request)) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    requests.push_back(std::move(request));
                }
                answered.notify_all();
                return int32_t(TegraGM20BCompileFailed);
            }
            std::map<std::string, gm20b::AirHash> hashes;
            const std::string &library = request.libraries[request.stages[0].library].bytes;
            if (!gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(library.data()), library.size(), hashes))
                return int32_t(TegraGM20BCompileUnavailable);
            bool own = false;
            const std::string key = gm20b::requestPackKey(request, own);
            const gm20b::CacheKey cache = gm20b::renderCacheKey(request.stack, key, hashes[request.stages[0].function],
                                                                hashes[request.stages[1].function], {}, own);
            gm20b::RuntimeRecord record;
            record.keyString = key;
            record.pipeline.keyHash = gm20b::Pack::fnv(key.data(), key.size());
            record.pipeline.vertexSlot = 0xff;
            record.pipeline.setBytes = 32;
            record.pipeline.memoryEntries = 3;
            gm20b::setEntryCount(record.pipeline, 2);
            record.pipeline.entry[0] = {0, gm20b::EntryTexture, 1, 0, gm20b::SamplerSlot, 0, 0};
            record.pipeline.entry[1] = {16, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0};
            if (request.fields.depthFormat) {
                gm20b::setEntryCount(record.pipeline, 3);
                record.pipeline.entry[2] = {32, gm20b::EntryFetch, 2, 0, gm20b::SamplerNone, 0, 0};
                record.pipeline.setBytes = 48;
                record.pipeline.memoryEntries = 7;
            }
            record.pipeline.flags = 0;
            for (auto &range : record.pipeline.uniform)
                range.entry = gm20b::kNoUniform;
            for (uint32_t stage = 0; stage < 2; stage++) {
                auto &program = record.programs[stage];
                program.image.assign(144, 0);
                program.image[80] = uint8_t(stage + 1);
                for (const auto &sampler : request.variant.cubic.samplers)
                    if (sampler.stage == stage + 1)
                        program.image[80] += 8;
                for (const auto &texture : request.variant.cubic.textures)
                    if (texture.stage == stage + 1)
                        program.image[81] = uint8_t(texture.format);
                program.image[82] = request.variant.fragmentTests;
                program.id = std::to_string(gm20b::Pack::fnv(program.image.data(), program.image.size()));
                program.gprs = 8;
                program.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
            }
            record.blob = {0x1a00, 0x1111};
            record.splitTried = true;
            if (shapes)
                shapes(request, record);
            const bool refused = refuses && refuses(request);
            const std::string entry = refused ? gm20b::encodeEntry(cache, nullptr, "refused by the test's compiler")
                                              : gm20b::encodeEntry(cache, &record, "");
            std::vector<gm20b::LibraryIdentity> ids;
            for (const auto &source : request.libraries)
                ids.push_back(gm20b::identityOf(source));
            {
                std::lock_guard<std::mutex> lock(mutex);
                store.put(gm20b::storeName(cache, request, ids).data(), reinterpret_cast<const uint8_t *>(entry.data()),
                          uint32_t(entry.size()));
                requests.push_back(std::move(request));
            }
            answered.notify_all();
            return int32_t(refused ? TegraGM20BCompileRefused : TegraGM20BCompileMade);
        };
        return true;
    }

    bool wait(size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return answered.wait_for(lock, std::chrono::seconds(2), [&] {
            return requests.size() >= count;
        });
    }

    std::vector<gm20b::CompileRequest> captured()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return requests;
    }

    // last: goes first, before what the compiler hook captures, which the device may still call
    kernel_model::HooksCleared cleared;
};

// What `variant` settles to, once it has (Pending: not within 2 s)
inline gm20b::Variant<gm20b::Pipeline>::State settled(gm20b::Variant<gm20b::Pipeline> &variant)
{
    auto signal = std::make_shared<std::promise<void>>();
    auto future = signal->get_future();
    const auto state = variant.observe([signal] {
        signal->set_value();
    });
    if (state == gm20b::Variant<gm20b::Pipeline>::State::Pending &&
        future.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
        return gm20b::Variant<gm20b::Pipeline>::State::Pending;
    return variant.state();
}

inline bool ready(gm20b::Variant<gm20b::Pipeline> &variant)
{
    return settled(variant) == gm20b::Variant<gm20b::Pipeline>::State::Ready;
}

inline uint32_t program(const std::vector<uint32_t> &words, uint32_t stage)
{
    std::vector<switch_gm20b_method> methods(words.size());
    const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
    EXPECT_GE(count, 0);
    uint32_t result = UINT32_MAX;
    for (long i = 0; i < count; i++)
        if (methods[i].subchannel == 0 && methods[i].method == NVB197_SET_PIPELINE_PROGRAM(stage == 1 ? 1 : 5))
            result = methods[i].value;
    return result;
}

inline std::vector<uint32_t> drawnPrograms(size_t firstJob, uint32_t stage)
{
    std::vector<uint32_t> result;
    uint32_t current = UINT32_MAX;
    const uint32_t first = NVB197_CALL_MME_MACRO(0), last = NVB197_CALL_MME_MACRO(127);
    for (size_t job = 0; job < kernel_model::model.jobs.size(); job++) {
        const auto &words = kernel_model::model.jobs[job].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        EXPECT_GE(count, 0);
        for (long i = 0; i < count; i++) {
            const auto &method = methods[i];
            if (method.subchannel != 0)
                continue;
            if (method.method == NVB197_SET_PIPELINE_PROGRAM(stage == 1 ? 1 : 5))
                current = method.value;
            const bool macro = method.method >= first && method.method <= last && (method.method - first) % 8 == 0 &&
                               gm20b::mme::countedAsDraw((method.method - first) / 8);
            if (job >= firstJob && (macro || method.method == NVB197_BEGIN))
                result.push_back(current);
        }
    }
    return result;
}

inline id<MTLTexture> image(id<MTLDevice> device, MTLTextureUsage usage,
                            MTLPixelFormat format = MTLPixelFormatBGRA8Unorm)
{
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                          width:64
                                                                                         height:64
                                                                                      mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = usage;
    return [device newTextureWithDescriptor:descriptor];
}

inline void sampler(id<MTLRenderCommandEncoder> encoder, uint32_t stage, id<MTLSamplerState> state,
                    NSUInteger index = 0)
{
    if (stage == 1)
        [encoder setVertexSamplerState:state atIndex:index];
    else
        [encoder setFragmentSamplerState:state atIndex:index];
}

inline void texture(id<MTLRenderCommandEncoder> encoder, uint32_t stage, id<MTLTexture> value)
{
    if (stage == 1)
        [encoder setVertexTexture:value atIndex:0];
    else
        [encoder setFragmentTexture:value atIndex:0];
}

}  // namespace harness
