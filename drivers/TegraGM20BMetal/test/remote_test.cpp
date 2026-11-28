// SPDX-License-Identifier: MIT
// remote.h over a model of the kernel's compiler methods (TegraGM20BCompilerABI.h): the store mapped
// once and read with no call; a state the store lacks asked of the compiler, its record read once it answers (the store
// mapped again for a new generation); a request waits for a compiler on its way, and a process the kernel serves no
// compiler is told so without a call. Asked asynchronously: each answer reaches its own callback (two requests the
// compiler answers from one compile, a compiler lost while they wait, an Unavailable end), a request past the
// connection's bound is held until an earlier one is answered, and a remote that goes ends what it still waits for.
#include "../remote.h"
#include "../answer_class.h"
#include "../cache_io.h"
#include "../obtained.h"
#include "../program_pages.h"
#include "../store_key.h"
#include "TegraGM20BABI.h"
#include "TegraGM20BCompilerABI.h"

#include <gtest/gtest.h>

#include <IOKit/IOReturn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/OSMessageNotification.h>
#include <mach/mach.h>
#include <pthread/qos.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kStack = 0x5357495443480005ull;

class Kernel {
public:
    std::mutex mu;
    uint32_t flags = TegraGM20BCompilerCapable | TegraGM20BCompilerRegistered | TegraGM20BCompilerStore;
    int called = kIOReturnSuccess, asyncCalled = kIOReturnSuccess, infoCalled = kIOReturnSuccess;
    int waitCalled = kIOReturnSuccess, registerLibraryCalled = kIOReturnSuccess;
    unsigned infos = 0;
    int32_t answer = TegraGM20BCompileMade;
    uint32_t generation = 1, how = TegraGM20BCompileCompiled | 4u << 8;
    TegraGM20BAbiCompileTimes times = {};  // the compiler's, as its Complete gave them
    unsigned maps = 0, compiles = 0, asyncAsked = 0, registerLibraries = 0;
    uint64_t nextLibraryId = 1;
    std::vector<uint64_t> compileWaits;
    std::function<int(uint64_t)> onCompileWait;
    std::function<void(const TegraGM20BAbiCompilerRegisterLibrary &)> onRegisterLibrary;
    std::function<void(const TegraGM20BAbiCompile &)> onAsyncCompile;
    std::function<void()> onCompilerInfo;
    std::function<void()> compiling;  // what the compiler does before it answers
    switch_seed_pack::StoreWriter store;

    // An asynchronous request as the kernel keeps it: its cookie, the callback its reference names and the port its answer
    // goes to
    struct Waiting {
        uint64_t cookie;
        uint64_t reference[kOSAsyncRef64Count];
        uint32_t port = 0;
        uint32_t qos = 0, flags = 0;
    };

    std::vector<Waiting> waiting;
    size_t bound = SIZE_MAX;  // asynchronous requests the connection takes before kIOReturnNoResources

    // The oldest waiting request finished: `result` the call's (kIOReturnSuccess, or kIOReturnAborted for one ended
    // without an answer), `answer` the compiler's
    void deliver(int result, int32_t answer)
    {
        Waiting w;
        uint32_t gen = 0;
        TegraGM20BAbiCompileTimes t = {};
        {
            std::lock_guard<std::mutex> l(mu);
            w = waiting.front();
            waiting.erase(waiting.begin());
            gen = generation;
            t = times;
        }
        const auto callback = reinterpret_cast<IOAsyncCallback>(uintptr_t(w.reference[kIOAsyncCalloutFuncIndex]));
        uint64_t words[TegraGM20BCompileAnswerWords] = {
            w.cookie,
            uint64_t(uint32_t(answer)) | uint64_t(gen) << 32,
            t.compile_start,
            t.compile_end,
            t.answered,
            uint64_t(t.answer_priority) | uint64_t(t.reserved) << 32};
        callback(reinterpret_cast<void *>(uintptr_t(w.reference[kIOAsyncCalloutRefconIndex])), result,
                 reinterpret_cast<void **>(words), TegraGM20BCompileAnswerWords);
    }

    // The oldest waiting request answered as the kernel answers it: IOUserClient's completion message to the request's own
    // port, which carries no class
    void send(int32_t answer)
    {
        Waiting w;
        uint32_t gen = 0;
        {
            std::lock_guard<std::mutex> l(mu);
            w = waiting.front();
            waiting.erase(waiting.begin());
            gen = generation;
        }
        uint64_t words[TegraGM20BCompileAnswerWords] = {w.cookie, uint64_t(uint32_t(answer)) | uint64_t(gen) << 32};
        const IOReturn result = kIOReturnSuccess;
        alignas(8)
            uint8_t bytes[sizeof(mach_msg_header_t) + sizeof(OSNotificationHeader64) + sizeof result + sizeof words] =
                {};
        auto *header = reinterpret_cast<mach_msg_header_t *>(bytes);
        auto *notification = reinterpret_cast<OSNotificationHeader64 *>(bytes + sizeof *header);
        uint8_t *content = bytes + sizeof *header + sizeof *notification;
        header->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
        header->msgh_size = sizeof bytes;
        header->msgh_remote_port = w.port;
        header->msgh_id = kOSNotificationMessageID;
        notification->size = sizeof result + sizeof words;
        notification->type = kIOAsyncCompletionNotificationType;
        memcpy(notification->reference, w.reference, sizeof w.reference);
        memcpy(content, &result, sizeof result);
        memcpy(content + sizeof result, words, sizeof words);
        ASSERT_EQ(
            mach_msg(header, MACH_SEND_MSG, sizeof bytes, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL),
            KERN_SUCCESS);
    }

    gm20b::Remote::Call call()
    {
        return [this](uint32_t method, const void *in, size_t, void *out, size_t outSize, uint64_t *reference,
                      uint32_t port) -> int {
            if (method == TegraGM20BMethodCompile && reference) {
                const auto req = *static_cast<const TegraGM20BAbiCompile *>(in);
                std::function<void(const TegraGM20BAbiCompile &)> hook;
                {
                    std::lock_guard<std::mutex> l(mu);
                    hook = onAsyncCompile;
                }
                if (hook)
                    hook(req);
                std::lock_guard<std::mutex> l(mu);
                if (waiting.size() >= bound)
                    return kIOReturnNoResources;
                asyncAsked++;
                if (asyncCalled != kIOReturnSuccess)
                    return asyncCalled;
                compiles++;
                Waiting w = {req.cookie, {}, port, req.qos, req.flags};
                memcpy(w.reference, reference, sizeof w.reference);
                waiting.push_back(w);
                return kIOReturnSuccess;
            }
            switch (method) {
            case TegraGM20BMethodCompilerInfo: {
                std::function<void()> hook;
                {
                    std::lock_guard<std::mutex> l(mu);
                    hook = onCompilerInfo;
                }
                if (hook)
                    hook();
                std::lock_guard<std::mutex> l(mu);
                infos++;
                if (infoCalled != kIOReturnSuccess)
                    return infoCalled;
                *static_cast<TegraGM20BAbiCompilerInfoReply *>(out) = {
                    store.mappedBytes(), kStack, flags, generation, {}};
                return kIOReturnSuccess;
            }
            case TegraGM20BMethodMap: {
                std::lock_guard<std::mutex> l(mu);
                maps++;
                *static_cast<TegraGM20BAbiMapReply *>(out) = {uint64_t(uintptr_t(store.base())), store.mappedBytes()};
                return kIOReturnSuccess;
            }
            case TegraGM20BMethodCompile: {
                std::function<void()> hook;
                {
                    std::lock_guard<std::mutex> l(mu);
                    compiles++;
                    if (outSize != sizeof(TegraGM20BAbiCompileReply))
                        return kIOReturnBadArgument;
                    hook = compiling;
                }
                if (hook)
                    hook();
                std::lock_guard<std::mutex> l(mu);
                *static_cast<TegraGM20BAbiCompileReply *>(out) = {answer, generation, how, 0, times};
                return called;
            }
            case TegraGM20BMethodCompileWait: {
                const auto &req = *static_cast<const TegraGM20BAbiCompileWait *>(in);
                std::function<int(uint64_t)> hook;
                {
                    std::lock_guard<std::mutex> l(mu);
                    hook = onCompileWait;
                }
                if (hook)
                    return hook(req.cookie);
                std::lock_guard<std::mutex> l(mu);
                compileWaits.push_back(req.cookie);
                return waitCalled;
            }
            case TegraGM20BMethodCompilerRegisterLibrary: {
                const auto req = *static_cast<const TegraGM20BAbiCompilerRegisterLibrary *>(in);
                std::function<void(const TegraGM20BAbiCompilerRegisterLibrary &)> hook;
                {
                    std::lock_guard<std::mutex> l(mu);
                    hook = onRegisterLibrary;
                }
                if (hook)
                    hook(req);
                std::lock_guard<std::mutex> l(mu);
                registerLibraries++;
                if (registerLibraryCalled != kIOReturnSuccess)
                    return registerLibraryCalled;
                *static_cast<TegraGM20BAbiCompilerRegisterLibraryReply *>(out) = {nextLibraryId++};
                return kIOReturnSuccess;
            }
            default:
                return kIOReturnUnsupported;
            }
        };
    }
};

class RemoteCompile : public testing::Test {
protected:
    std::filesystem::path root;
    Kernel kernel;
    gm20b::Digest name{};

    void SetUp() override
    {
        std::string pattern = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/remote-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
        open(kStack);
        name.fill(0x42);
    }

    void TearDown() override
    {
        std::filesystem::remove_all(root);
    }

    void open(uint64_t stack)
    {
        kernel.store.close();
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        memcpy(identity, &stack, sizeof stack);
        identity[8] = 0x99;  // the pack rules' part, which clients leave to the compiler
        std::string why;
        ASSERT_TRUE(kernel.store.open((root / std::to_string(stack)).string(), identity, 64, 1u << 20, why)) << why;
    }

    void publish(const std::string &bytes = "a record")
    {
        kernel.store.put(name.data(), reinterpret_cast<const uint8_t *>(bytes.data()), uint32_t(bytes.size()));
    }
};

TEST_F(RemoteCompile, AStoredRecordIsReadWithNoRequestFromAStoreMappedOnce)
{
    publish();
    gm20b::Remote remote(kernel.call(), kStack);
    ASSERT_TRUE(remote.serving());
    std::string bytes;
    for (int i = 0; i < 100; ++i)
        ASSERT_TRUE(remote.find(name, bytes));
    EXPECT_EQ(bytes, "a record");
    EXPECT_EQ(kernel.maps, 1u);
    EXPECT_EQ(kernel.compiles, 0u);
}

// A program slot the compiler published on pages of its own is found in place in the mapping, by its bytes alone
TEST_F(RemoteCompile, APublishedSlotIsFoundInPlaceInTheMapping)
{
    std::vector<uint8_t> slot(40 * 1024);
    for (size_t i = 0; i < slot.size(); i++)
        slot[i] = uint8_t(i * 7);
    std::vector<uint8_t> entry = slot;
    entry.resize(gm20b::programSlotEntryBytes(slot.size()));
    publish();
    const gm20b::Digest slotName = gm20b::programSlotName(slot.data(), slot.size());
    kernel.store.put(slotName.data(), entry.data(), uint32_t(entry.size()));
    gm20b::Remote remote(kernel.call(), kStack);
    std::string bytes;
    ASSERT_TRUE(remote.find(name, bytes)) << "a record lookup maps the store";
    const uint8_t *mapping = nullptr;
    uint64_t mapped = 0;
    const uint8_t *at = remote.findSlot(slot.data(), slot.size(), mapping, mapped);
    ASSERT_NE(at, nullptr);
    EXPECT_GE(at, mapping);
    EXPECT_LE(at + entry.size(), mapping + mapped);
    EXPECT_EQ(uint64_t(at - mapping) % gm20b::kProgramSlotPage, 0u);
    EXPECT_EQ(memcmp(at, slot.data(), slot.size()), 0);
    slot[100] ^= 1;
    EXPECT_EQ(remote.findSlot(slot.data(), slot.size(), mapping, mapped), nullptr) << "other bytes are another slot";
}

TEST_F(RemoteCompile, ARecordTheCompilerMadeIsReadOnceItAnswersFromItsNewGeneration)
{
    gm20b::Remote remote(kernel.call(), kStack);
    std::string bytes;
    EXPECT_FALSE(remote.find(name, bytes));
    kernel.compiling = [&] {
        publish();
        kernel.generation = 2;
    };
    gm20b::Remote::Answered answered;
    EXPECT_EQ(remote.compile("a request", &answered), gm20b::Remote::Answer::Made);
    EXPECT_EQ(answered.how, kernel.how);
    EXPECT_TRUE(remote.find(name, bytes));
    EXPECT_EQ(kernel.maps, 2u);
    EXPECT_TRUE(remote.find(name, bytes));
    EXPECT_EQ(kernel.maps, 2u);
}

TEST_F(RemoteCompile, ARequestWaitsForACompilerOnItsWay)
{
    kernel.flags = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected;
    gm20b::Remote remote(kernel.call(), kStack);
    EXPECT_TRUE(remote.serving());
    EXPECT_EQ(kernel.maps, 0u);
    EXPECT_EQ(remote.compile("a request"), gm20b::Remote::Answer::Made);
    EXPECT_EQ(kernel.compiles, 1u);
}

TEST_F(RemoteCompile, AProcessServedNoCompilerIsToldSoWithNoCall)
{
    kernel.flags = TegraGM20BCompilerCapable;
    gm20b::Remote remote(kernel.call(), kStack);
    EXPECT_FALSE(remote.serving());
    EXPECT_EQ(remote.compile("a request"), gm20b::Remote::Answer::Unavailable);
    EXPECT_FALSE(
        remote.compileAsync("a request", true, 0, [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {}));
    EXPECT_EQ(kernel.compiles, 0u);
    kernel.flags = 0;
    gm20b::Remote older(kernel.call(), kStack);
    EXPECT_FALSE(older.serving());
}

TEST_F(RemoteCompile, AProcessStartedBeforeTheCompilerIsServedOnceItRegisters)
{
    kernel.flags = TegraGM20BCompilerCapable;
    gm20b::Remote remote(kernel.call(), kStack);
    EXPECT_FALSE(remote.serving());
    publish();
    kernel.flags = TegraGM20BCompilerCapable | TegraGM20BCompilerRegistered | TegraGM20BCompilerStore;
    EXPECT_TRUE(remote.serving());
    std::string bytes;
    EXPECT_TRUE(remote.find(name, bytes));
}

TEST_F(RemoteCompile, AFirstCompilerInfoThatFailedIsAskedAgainByALaterAsk)
{
    publish();
    kernel.infoCalled = kIOReturnNoMemory;
    gm20b::Remote remote(kernel.call(), kStack);
    EXPECT_FALSE(remote.serving());
    kernel.infoCalled = kIOReturnSuccess;
    EXPECT_TRUE(remote.serving());
    std::string bytes;
    EXPECT_TRUE(remote.find(name, bytes));
    EXPECT_EQ(remote.compile("a request"), gm20b::Remote::Answer::Made);
}

// A failed call fails its own ask: no count, no latch, each later ask calls afresh
TEST_F(RemoteCompile, EveryAskAfterAFailedCompilerInfoCallsAfresh)
{
    kernel.infoCalled = kIOReturnNotResponding;
    gm20b::Remote remote(kernel.call(), kStack);
    for (int i = 0; i < 20; ++i)
        EXPECT_FALSE(remote.serving());
    EXPECT_EQ(kernel.infos, 21u);
    kernel.infoCalled = kIOReturnSuccess;
    EXPECT_TRUE(remote.serving());
}

TEST_F(RemoteCompile, AKernelWithoutTheCompilersMethodsIsAskedOnce)
{
    for (const int absent : {int(kIOReturnUnsupported), int(kIOReturnNotOpen)}) {
        kernel.infos = 0;
        kernel.infoCalled = absent;
        gm20b::Remote remote(kernel.call(), kStack);
        for (int i = 0; i < 4; ++i)
            EXPECT_FALSE(remote.serving());
        EXPECT_FALSE(remote.declined());
        EXPECT_EQ(kernel.infos, 1u) << std::hex << absent;
    }
    kernel.infos = 0;
    kernel.infoCalled = kIOReturnSuccess;
    kernel.flags = 0;
    gm20b::Remote older(kernel.call(), kStack);
    EXPECT_FALSE(older.serving());
    EXPECT_EQ(kernel.infos, 1u);
}

TEST_F(RemoteCompile, AnInterruptedWaitOrAFullQueueIsAFailureALaterAskRetries)
{
    gm20b::Remote remote(kernel.call(), kStack);
    kernel.called = kIOReturnAborted;
    EXPECT_EQ(remote.compile("a request"), gm20b::Remote::Answer::Failed);
    kernel.called = kIOReturnNoResources;
    EXPECT_EQ(remote.compile("a request"), gm20b::Remote::Answer::Failed);
    kernel.called = kIOReturnSuccess;
    kernel.answer = TegraGM20BCompileUnavailable;
    EXPECT_EQ(remote.compile("a request"), gm20b::Remote::Answer::Unavailable);
}

// The third compiler lost: none registered, none waited for. The process keeps reading the store and making the states
// it lacks off the asking thread, as for any request a compiler did not take, but sends no request until one registers.
TEST_F(RemoteCompile, AProcessSendsNoRequestOnceOneFindsNoCompilerUntilOneRegisters)
{
    gm20b::Remote remote(kernel.call(), kStack);
    ASSERT_TRUE(remote.serving());
    kernel.flags = TegraGM20BCompilerCapable | TegraGM20BCompilerStore;
    kernel.answer = TegraGM20BCompileUnavailable;
    EXPECT_EQ(remote.compile("a request"), gm20b::Remote::Answer::Unavailable);
    EXPECT_TRUE(remote.serving());
    EXPECT_FALSE(remote.taking());
    EXPECT_EQ(remote.compile("a request"), gm20b::Remote::Answer::Unavailable);
    EXPECT_FALSE(
        remote.compileAsync("a request", true, 0, [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {}));
    EXPECT_EQ(kernel.compiles, 1u);
    publish();
    std::string bytes;
    EXPECT_TRUE(remote.find(name, bytes));
    kernel.flags = TegraGM20BCompilerCapable | TegraGM20BCompilerRegistered | TegraGM20BCompilerStore;
    kernel.answer = TegraGM20BCompileMade;
    EXPECT_TRUE(remote.taking());
    EXPECT_EQ(remote.compile("a request"), gm20b::Remote::Answer::Made);
    EXPECT_EQ(kernel.compiles, 2u);
}

TEST_F(RemoteCompile, AnAsynchronousRequestRefusedForWantOfACompilerStopsTheRequests)
{
    gm20b::Remote remote(kernel.call(), kStack);
    kernel.flags = TegraGM20BCompilerCapable;
    kernel.asyncCalled = kIOReturnNotReady;
    EXPECT_FALSE(
        remote.compileAsync("a request", true, 0, [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {}));
    EXPECT_TRUE(remote.serving());
    EXPECT_FALSE(remote.taking());
    EXPECT_FALSE(
        remote.compileAsync("a request", true, 0, [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {}));
    EXPECT_EQ(kernel.compiles, 0u);
    EXPECT_EQ(kernel.asyncAsked, 1u);
}

// A compiler that went with requests waiting answers them Unavailable: while the next one is on its way the process asks
// again at once
TEST_F(RemoteCompile, AnUnavailableAnswerWhileTheNextCompilerIsWaitedForKeepsTheRequests)
{
    gm20b::Remote remote(kernel.call(), kStack);
    kernel.flags = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected;
    kernel.answer = TegraGM20BCompileUnavailable;
    EXPECT_EQ(remote.compile("a request"), gm20b::Remote::Answer::Unavailable);
    EXPECT_TRUE(remote.taking());
    EXPECT_EQ(remote.compile("a request"), gm20b::Remote::Answer::Unavailable);
    EXPECT_EQ(kernel.compiles, 2u);
}

TEST_F(RemoteCompile, AStoreOfAnotherStackIsNotRead)
{
    open(kStack + 1);
    publish();
    gm20b::Remote remote(kernel.call(), kStack);
    std::string bytes;
    EXPECT_FALSE(remote.find(name, bytes));
}

// The answers callbacks got, in the order they came
struct Answers {
    std::vector<gm20b::Remote::Answer> got;
    std::vector<gm20b::Remote::Answered> with;

    gm20b::Remote::Done next()
    {
        return [this](gm20b::Remote::Answer a, const gm20b::Remote::Answered &answered) {
            got.push_back(a);
            with.push_back(answered);
        };
    }
};

TEST_F(RemoteCompile, AnAsynchronousAnswerReachesItsCallbackWithItsGeneration)
{
    gm20b::Remote remote(kernel.call(), kStack);
    Answers answers;
    ASSERT_TRUE(remote.compileAsync("a request", true, 0x21, answers.next()));
    EXPECT_TRUE(answers.got.empty());
    publish();
    kernel.generation = 2;
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    ASSERT_EQ(answers.got.size(), 1u);
    EXPECT_EQ(answers.got[0], gm20b::Remote::Answer::Made);
    std::string bytes;
    EXPECT_TRUE(remote.find(name, bytes));
    EXPECT_EQ(kernel.maps, 2u);
}

TEST_F(RemoteCompile, TwoRequestsTheCompilerAnswersFromOneCompileBothReadIt)
{
    gm20b::Remote remote(kernel.call(), kStack);
    Answers first, second;
    ASSERT_TRUE(remote.compileAsync("a request", true, 0x21, first.next()));
    ASSERT_TRUE(remote.compileAsync("a request", true, 0x19, second.next()));
    EXPECT_EQ(kernel.compiles, 2u);
    publish();
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    ASSERT_EQ(first.got.size(), 1u);
    ASSERT_EQ(second.got.size(), 1u);
    EXPECT_EQ(first.got[0], gm20b::Remote::Answer::Made);
    EXPECT_EQ(second.got[0], gm20b::Remote::Answer::Made);
    std::string bytes;
    EXPECT_TRUE(remote.find(name, bytes));
}

TEST_F(RemoteCompile, ARequestWhoseCompilerWentEndsUnavailableOrFailed)
{
    gm20b::Remote remote(kernel.call(), kStack);
    Answers lost, declined, twice;
    ASSERT_TRUE(remote.compileAsync("a request", true, 0x21, lost.next()));
    ASSERT_TRUE(remote.compileAsync("another", true, 0x21, declined.next()));
    ASSERT_TRUE(remote.compileAsync("a third", true, 0x21, twice.next()));
    kernel.deliver(kIOReturnAborted, 0);
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileUnavailable);
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileFailed);
    EXPECT_EQ(lost.got, std::vector<gm20b::Remote::Answer>{gm20b::Remote::Answer::Unavailable});
    EXPECT_EQ(declined.got, std::vector<gm20b::Remote::Answer>{gm20b::Remote::Answer::Unavailable});
    EXPECT_EQ(twice.got, std::vector<gm20b::Remote::Answer>{gm20b::Remote::Answer::Failed});
}

TEST_F(RemoteCompile, ARequestPastTheBoundIsSentOnceAnEarlierOneIsAnswered)
{
    kernel.bound = 1;
    gm20b::Remote remote(kernel.call(), kStack);
    Answers first, held;
    ASSERT_TRUE(remote.compileAsync("a request", true, 0x21, first.next()));
    ASSERT_TRUE(remote.compileAsync("another", true, 0x21, held.next()));
    EXPECT_EQ(kernel.waiting.size(), 1u);
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    EXPECT_EQ(first.got.size(), 1u);
    ASSERT_EQ(kernel.waiting.size(), 1u);
    EXPECT_TRUE(held.got.empty());
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileRefused);
    EXPECT_EQ(held.got, std::vector<gm20b::Remote::Answer>{gm20b::Remote::Answer::Refused});
}

TEST_F(RemoteCompile, ARemoteThatGoesEndsWhatItWaitsFor)
{
    Answers waiting;
    {
        gm20b::Remote remote(kernel.call(), kStack);
        ASSERT_TRUE(remote.compileAsync("a request", true, 0x21, waiting.next()));
    }
    EXPECT_EQ(waiting.got, std::vector<gm20b::Remote::Answer>{gm20b::Remote::Answer::Unavailable});
}

// A debug compiler's account of the wait reaches the asker whole, answered either way, and splits the asker's wait
TEST_F(RemoteCompile, TheCompilersTimesReachTheAskerAndSplitItsWait)
{
    kernel.times = {2'000'000, 47'000'000, 48'000'000, 37, 0};
    publish();
    gm20b::Remote remote(kernel.call(), kStack);
    gm20b::Remote::Answered answered;
    ASSERT_EQ(remote.compile("a request", &answered), gm20b::Remote::Answer::Made);
    EXPECT_EQ(answered.how, kernel.how);
    EXPECT_EQ(answered.times.compile_start, 2'000'000u);
    EXPECT_EQ(answered.times.compile_end, 47'000'000u);
    EXPECT_EQ(answered.times.answered, 48'000'000u);
    EXPECT_EQ(answered.times.answer_priority, 37u);
    EXPECT_NE(answered.at, 0u);

    Answers later;
    ASSERT_TRUE(remote.compileAsync("a request", true, 0x21, later.next()));
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    ASSERT_EQ(later.with.size(), 1u);
    EXPECT_EQ(later.with[0].times.compile_end, 47'000'000u);
    EXPECT_EQ(later.with[0].times.answered, 48'000'000u);
    EXPECT_EQ(later.with[0].times.answer_priority, 37u);

    gm20b::Obtained o;
    o.split(1'000, 2'000'000, 47'000'000, 48'000'000, 48'500'000, 49'000);
    EXPECT_EQ(o.queueUs, 1'000);
    EXPECT_EQ(o.compileUs, 45'000);
    EXPECT_EQ(o.answerUs, 1'000);
    EXPECT_EQ(o.wakeUs, 500);
    EXPECT_EQ(o.storeUs, 500);
    gm20b::Obtained stored;
    stored.split(1'000, 0, 0, 0, 48'500'000, 49'000);
    EXPECT_EQ(stored.queueUs, -1);
    EXPECT_EQ(stored.answerUs, -1);
    EXPECT_EQ(stored.storeUs, 500);
}

// Boot 22: an asynchronous answer was taken on one queue of the user-initiated class whatever its asker's, an interactive
// asker's one class below it. Each answer is now sent to a port of its asker's class and taken there; a thread of fixed
// priority counts as interactive.
TEST_F(RemoteCompile, AnAsynchronousAnswerIsTakenAtItsAskersClass)
{
    publish();
    gm20b::Remote remote(kernel.call(), kStack);

    struct Taken {
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        qos_class_t at = QOS_CLASS_UNSPECIFIED;
    };

    const qos_class_t asked[] = {QOS_CLASS_USER_INTERACTIVE, QOS_CLASS_BACKGROUND, QOS_CLASS_UNSPECIFIED,
                                 QOS_CLASS_UTILITY};
    Taken taken[std::size(asked)];
    for (size_t i = 0; i < std::size(asked); ++i)
        ASSERT_TRUE(remote.compileAsync("a request", true, asked[i],
                                        [&taken, i](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {
                                            taken[i].at = qos_class_self();
                                            dispatch_semaphore_signal(taken[i].done);
                                        }));
    ASSERT_EQ(kernel.waiting.size(), std::size(asked));
    EXPECT_NE(kernel.waiting[0].port, kernel.waiting[1].port);
    EXPECT_EQ(kernel.waiting[0].port, kernel.waiting[2].port) << "a fixed-priority asker is answered as interactive";
    for (size_t i = 0; i < std::size(asked); ++i)
        kernel.send(TegraGM20BCompileMade);
    for (size_t i = 0; i < std::size(asked); ++i) {
        ASSERT_EQ(dispatch_semaphore_wait(taken[i].done, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))),
                  0)
            << "answer " << i << " never taken";
        EXPECT_EQ(taken[i].at, gm20b::answerClass(asked[i])) << "answer " << i;
    }
}

TEST_F(RemoteCompile, AnUnwaitedAsynchronousRequestRunsAtTheBackgroundClass)
{
    publish();
    gm20b::Remote remote(kernel.call(), kStack);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    qos_class_t unwaitedAt = QOS_CLASS_UNSPECIFIED;
    ASSERT_TRUE(remote.compileAsync("unwaited", false, QOS_CLASS_USER_INTERACTIVE,
                                    [&](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {
                                        unwaitedAt = qos_class_self();
                                        dispatch_semaphore_signal(done);
                                    }));
    ASSERT_TRUE(remote.compileAsync("waited", true, QOS_CLASS_USER_INTERACTIVE,
                                    [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {}));
    ASSERT_EQ(kernel.waiting.size(), 2u);
    EXPECT_EQ(kernel.waiting[0].flags & TegraGM20BCompileWaited, 0u);
    EXPECT_EQ(kernel.waiting[0].qos, uint32_t(QOS_CLASS_BACKGROUND));
    EXPECT_NE(kernel.waiting[1].flags & TegraGM20BCompileWaited, 0u);
    EXPECT_EQ(kernel.waiting[1].qos, uint32_t(QOS_CLASS_USER_INTERACTIVE));
    EXPECT_NE(kernel.waiting[0].port, kernel.waiting[1].port);
    kernel.send(TegraGM20BCompileMade);
    ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))), 0);
    EXPECT_EQ(unwaitedAt, QOS_CLASS_BACKGROUND);
}

TEST_F(RemoteCompile, WaitAsyncCallsCompileWaitForActiveAndHeldCookies)
{
    publish();
    kernel.bound = 1;
    gm20b::Remote remote(kernel.call(), kStack);
    uint64_t first = 0, held = 0;
    ASSERT_TRUE(remote.compileAsync(
        "first", true, QOS_CLASS_USER_INTERACTIVE, [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {},
        &first));
    ASSERT_TRUE(remote.compileAsync(
        "held", true, QOS_CLASS_USER_INTERACTIVE, [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {},
        &held));
    ASSERT_NE(first, 0u);
    ASSERT_NE(held, 0u);
    ASSERT_NE(first, held);

    // Waiting on an active cookie returns false on kIOReturnSuccess (no redundant retry needed).
    EXPECT_FALSE(remote.waitAsync(first));
    ASSERT_EQ(kernel.compileWaits.size(), 1u);
    EXPECT_EQ(kernel.compileWaits[0], first);

    // Waiting on an aborted active cookie returns true so the caller retries.
    kernel.waitCalled = kIOReturnAborted;
    EXPECT_TRUE(remote.waitAsync(first));
    ASSERT_EQ(kernel.compileWaits.size(), 2u);
    EXPECT_EQ(kernel.compileWaits[1], first);

    // Waiting on a held cookie pushes the oldest active pending cookie and returns true on success.
    kernel.waitCalled = kIOReturnSuccess;
    EXPECT_TRUE(remote.waitAsync(held));
    ASSERT_EQ(kernel.compileWaits.size(), 3u);
    EXPECT_EQ(kernel.compileWaits[2], first);

    // Once the first request completes and promotes `held`, waiting on `held` passes `held` directly.
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    EXPECT_FALSE(remote.waitAsync(held));
    ASSERT_EQ(kernel.compileWaits.size(), 4u);
    EXPECT_EQ(kernel.compileWaits[3], held);

    // An older kernel returning kIOReturnUnsupported disables further CompileWait syscalls.
    kernel.waitCalled = kIOReturnUnsupported;
    EXPECT_FALSE(remote.waitAsync(held));
    EXPECT_FALSE(remote.waitAsync(held));
    EXPECT_EQ(kernel.compileWaits.size(), 5u);
}

TEST_F(RemoteCompile, WaitAsyncHeldCookieSurvivesNotFoundRaceAndFollowsPromotion)
{
    publish();
    kernel.bound = 2;
    gm20b::Remote remote(kernel.call(), kStack);
    uint64_t c1 = 0, c2 = 0, held = 0;
    ASSERT_TRUE(remote.compileAsync(
        "first", true, QOS_CLASS_USER_INTERACTIVE, [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {}, &c1));
    ASSERT_TRUE(remote.compileAsync(
        "second", true, QOS_CLASS_USER_INTERACTIVE, [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {},
        &c2));
    ASSERT_TRUE(remote.compileAsync(
        "held", true, QOS_CLASS_USER_INTERACTIVE, [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {},
        &held));

    std::mutex waitLock;
    std::condition_variable waitCv;
    std::vector<uint64_t> seen;
    bool sawHeld = false;
    kernel.onCompileWait = [&](uint64_t cookie) -> int {
        std::lock_guard<std::mutex> l(waitLock);
        seen.push_back(cookie);
        if (cookie == held) {
            sawHeld = true;
            waitCv.notify_all();
            return kIOReturnSuccess;
        }
        waitCv.notify_all();
        // Kernel already unlinked active pending_ requests before Remote::delivered ran.
        return kIOReturnNotFound;
    };

    std::atomic<bool> waiterDone{false};
    bool retry = true;
    std::thread waiter([&] {
        retry = remote.waitAsync(held);
        waiterDone.store(true, std::memory_order_release);
    });

    {
        std::unique_lock<std::mutex> l(waitLock);
        ASSERT_TRUE(waitCv.wait_for(l, std::chrono::seconds(2), [&] {
            return seen.size() >= 2;
        }));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    EXPECT_FALSE(waiterDone.load(std::memory_order_acquire))
        << "waitAsync(held) returned early instead of waiting for Remote::delivered to promote held";

    // Delivering c1 promotes `held` into pending_ and wakes heldChanged_.
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    {
        std::unique_lock<std::mutex> l(waitLock);
        ASSERT_TRUE(waitCv.wait_for(l, std::chrono::seconds(2), [&] {
            return sawHeld;
        }));
    }
    waiter.join();
    EXPECT_FALSE(retry);
    EXPECT_EQ(seen, (std::vector<uint64_t>{c1, c2, held}));
}

TEST_F(RemoteCompile, RegisterLibraryCachesPerSliceAndFallsBackCleanly)
{
    std::string lib = "metallib-payload";
    constexpr uint64_t kSource = 1, kOtherSource = 2;

    {
        gm20b::Remote remote(kernel.call(), kStack);
        EXPECT_EQ(remote.registerLibrary(lib.data(), lib.size(), kSource), 0u);
        EXPECT_EQ(kernel.registerLibraries, 0u);
    }

    kernel.flags |= TegraGM20BCompilerLibraries;
    gm20b::Remote remote(kernel.call(), kStack);
    const uint64_t id = remote.registerLibrary(lib.data(), lib.size(), kSource);
    EXPECT_EQ(id, 1u);
    EXPECT_EQ(kernel.registerLibraries, 1u);
    EXPECT_EQ(remote.registerLibrary(lib.data(), lib.size(), kSource), 1u);
    EXPECT_EQ(kernel.registerLibraries, 1u);
    EXPECT_EQ(remote.registerLibrary(lib.data(), lib.size(), kOtherSource), 2u);
    EXPECT_EQ(kernel.registerLibraries, 2u);

    std::string lib2 = "another-library";
    constexpr uint64_t d2 = 3;
    kernel.registerLibraryCalled = kIOReturnNoResources;
    EXPECT_EQ(remote.registerLibrary(lib2.data(), lib2.size(), d2), 0u);
    kernel.registerLibraryCalled = kIOReturnSuccess;
    EXPECT_EQ(remote.registerLibrary(lib2.data(), lib2.size(), d2), 3u);

    std::string lib3 = "third-library";
    constexpr uint64_t d3 = 4;
    kernel.registerLibraryCalled = kIOReturnUnsupported;
    EXPECT_EQ(remote.registerLibrary(lib3.data(), lib3.size(), d3), 0u);
    const unsigned callsAfterUnsupported = kernel.registerLibraries;
    EXPECT_EQ(remote.registerLibrary(lib3.data(), lib3.size(), d3), 0u);
    EXPECT_EQ(kernel.registerLibraries, callsAfterUnsupported);
}

TEST_F(RemoteCompile, RegisterLibraryRunsDifferentKeysInParallelAndCoalescesSameKey)
{
    kernel.flags |= TegraGM20BCompilerLibraries;
    gm20b::Remote remote(kernel.call(), kStack);

    std::string libA = "library-A-payload", libB = "library-B-payload";
    std::mutex gateMu;
    std::condition_variable gateCv;
    bool aEntered = false, releaseA = false;
    kernel.onRegisterLibrary = [&](const TegraGM20BAbiCompilerRegisterLibrary &req) {
        if (req.address == uint64_t(uintptr_t(libA.data()))) {
            std::unique_lock<std::mutex> l(gateMu);
            aEntered = true;
            gateCv.notify_all();
            gateCv.wait(l, [&] {
                return releaseA;
            });
        }
    };

    uint64_t idA1 = 0, idA2 = 0, idB = 0;
    std::thread tA1([&] {
        idA1 = remote.registerLibrary(libA.data(), libA.size(), 1);
    });
    {
        std::unique_lock<std::mutex> l(gateMu);
        ASSERT_TRUE(gateCv.wait_for(l, std::chrono::seconds(5), [&] {
            return aEntered;
        }));
    }

    // Different key registers in parallel without blocking on key A's ioctl.
    idB = remote.registerLibrary(libB.data(), libB.size(), 2);
    EXPECT_NE(idB, 0u);

    // Same key coalesces onto tA1 without issuing a duplicate ioctl.
    std::thread tA2([&] {
        idA2 = remote.registerLibrary(libA.data(), libA.size(), 1);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    {
        std::lock_guard<std::mutex> l(gateMu);
        releaseA = true;
    }
    gateCv.notify_all();
    tA1.join();
    tA2.join();

    EXPECT_NE(idA1, 0u);
    EXPECT_EQ(idA1, idA2);
    EXPECT_NE(idA1, idB);
    EXPECT_EQ(kernel.registerLibraries, 2u);
}

TEST_F(RemoteCompile, AsyncCompileAndRecheckDropAsyncLockAllowingConcurrentDelivery)
{
    gm20b::Remote remote(kernel.call(), kStack);

    std::atomic<int> firstAnswered{0};
    ASSERT_TRUE(remote.compileAsync("first", true, QOS_CLASS_USER_INTERACTIVE,
                                    [&](gm20b::Remote::Answer a, const gm20b::Remote::Answered &) {
                                        if (a == gm20b::Remote::Answer::Made)
                                            firstAnswered.fetch_add(1, std::memory_order_release);
                                    }));

    std::mutex gateMu;
    std::condition_variable gateCv;
    bool secondEntered = false, releaseSecond = false;
    kernel.onAsyncCompile = [&](const TegraGM20BAbiCompile &req) {
        if (req.cookie == 2) {
            std::unique_lock<std::mutex> l(gateMu);
            secondEntered = true;
            gateCv.notify_all();
            gateCv.wait(l, [&] {
                return releaseSecond;
            });
        }
    };

    bool secondSent = false;
    std::thread asker([&] {
        secondSent = remote.compileAsync("second", true, QOS_CLASS_USER_INTERACTIVE,
                                         [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {});
    });
    {
        std::unique_lock<std::mutex> l(gateMu);
        ASSERT_TRUE(gateCv.wait_for(l, std::chrono::seconds(5), [&] {
            return secondEntered;
        }));
    }

    // Delivery of request 1 must complete immediately while request 2's async compile ioctl is blocked.
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    EXPECT_EQ(firstAnswered.load(std::memory_order_acquire), 1);

    {
        std::lock_guard<std::mutex> l(gateMu);
        releaseSecond = true;
    }
    gateCv.notify_all();
    asker.join();
    ASSERT_TRUE(secondSent);

    // Delivery of request 2 must also complete while a kIOReturnNotReady ask is blocked inside recheck().
    {
        std::lock_guard<std::mutex> l(kernel.mu);
        kernel.onAsyncCompile = nullptr;
        kernel.asyncCalled = kIOReturnNotReady;
    }
    bool recheckEntered = false, releaseRecheck = false;
    {
        std::lock_guard<std::mutex> l(kernel.mu);
        kernel.onCompilerInfo = [&] {
            std::unique_lock<std::mutex> g(gateMu);
            recheckEntered = true;
            gateCv.notify_all();
            gateCv.wait(g, [&] {
                return releaseRecheck;
            });
        };
    }
    bool thirdSent = true;
    std::thread notReadyAsker([&] {
        thirdSent = remote.compileAsync("third", true, QOS_CLASS_USER_INTERACTIVE,
                                        [](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {});
    });
    {
        std::unique_lock<std::mutex> l(gateMu);
        ASSERT_TRUE(gateCv.wait_for(l, std::chrono::seconds(5), [&] {
            return recheckEntered;
        }));
    }
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    {
        std::lock_guard<std::mutex> l(gateMu);
        releaseRecheck = true;
    }
    gateCv.notify_all();
    notReadyAsker.join();
    EXPECT_FALSE(thirdSent);
}

TEST_F(RemoteCompile, ConcurrentAsyncCompileDeliverAndRegisterLibraryUnderTSan)
{
    kernel.flags |= TegraGM20BCompilerLibraries;
    kernel.bound = 2;
    gm20b::Remote remote(kernel.call(), kStack);

    std::atomic<int> deliveredCount{0};
    std::atomic<int> submittedCount{0};
    std::vector<std::string> libs;
    for (int i = 0; i < 6; ++i)
        libs.push_back("metallib-" + std::to_string(i));

    std::vector<std::thread> askers;
    for (int t = 0; t < 4; ++t) {
        askers.emplace_back([&, t] {
            for (int i = 0; i < 12; ++i) {
                const std::string &lib = libs[size_t((i + t) % libs.size())];
                EXPECT_NE(remote.registerLibrary(lib.data(), lib.size(), uint64_t((i + t) % libs.size()) + 1), 0u);
                if (remote.compileAsync("req", true, QOS_CLASS_USER_INTERACTIVE,
                                        [&](gm20b::Remote::Answer, const gm20b::Remote::Answered &) {
                                            deliveredCount.fetch_add(1, std::memory_order_relaxed);
                                        }))
                    submittedCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::atomic<bool> askersDone{false};
    std::thread deliverer([&] {
        for (;;) {
            bool hasWaiting = false;
            {
                std::lock_guard<std::mutex> l(kernel.mu);
                hasWaiting = !kernel.waiting.empty();
            }
            if (hasWaiting) {
                kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
                continue;
            }
            if (askersDone.load(std::memory_order_acquire) &&
                deliveredCount.load(std::memory_order_acquire) >= submittedCount.load(std::memory_order_acquire))
                break;
            std::this_thread::yield();
        }
    });

    for (auto &a : askers)
        a.join();
    askersDone.store(true, std::memory_order_release);
    deliverer.join();
    EXPECT_EQ(deliveredCount.load(), submittedCount.load());
    EXPECT_EQ(kernel.registerLibraries, libs.size());
}

// Completion's queue (TegraGM20BMetalComplete.mm) runs each block at its asker's class, below its own default as well as above it,
// and in order
TEST(AnswerQueue, EachBlockRunsAtItsAskersClassInOrder)
{
    dispatch_queue_t queue = gm20b::answerQueue("answer-queue-test");
    const qos_class_t asked[] = {QOS_CLASS_BACKGROUND, QOS_CLASS_USER_INTERACTIVE, QOS_CLASS_UTILITY,
                                 QOS_CLASS_UNSPECIFIED};
    std::vector<qos_class_t> ran;
    std::vector<qos_class_t> *into = &ran;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    for (qos_class_t qos : asked)
        gm20b::asyncAt(queue, qos, ^{
            into->push_back(qos_class_self());
            dispatch_semaphore_signal(done);
        });
    for (size_t i = 0; i < std::size(asked); ++i)
        ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))), 0);
    ASSERT_EQ(ran.size(), std::size(asked));
    for (size_t i = 0; i < std::size(asked); ++i)
        EXPECT_EQ(ran[i], gm20b::answerClass(asked[i])) << "block " << i;
    dispatch_release(queue);
}

}  // namespace
