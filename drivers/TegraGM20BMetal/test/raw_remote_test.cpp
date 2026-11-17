// SPDX-License-Identifier: MIT
// raw/runtime/raw_remote.h over a model of the kernel's compiler methods (TegraGM20BCompilerABI.h): the store mapped
// once and read with no call; a state the store lacks asked of the compiler, its record read once it answers (the store
// mapped again for a new generation); a request waits for a compiler on its way, and a process the kernel serves no
// compiler is told so without a call. Asked asynchronously: each answer reaches its own callback (two requests the
// compiler answers from one compile, a compiler lost while they wait, an Unavailable end), a request past the
// connection's bound is held until an earlier one is answered, and a remote that goes ends what it still waits for.
#include "../raw/runtime/raw_remote.h"
#include "../raw/runtime/raw_answer_class.h"
#include "../raw/runtime/raw_cache_io.h"
#include "../raw/runtime/raw_obtained.h"
#include "../raw/runtime/raw_program_pages.h"
#include "TegraGM20BABI.h"
#include "TegraGM20BCompilerABI.h"

#include <gtest/gtest.h>

#include <IOKit/IOReturn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/OSMessageNotification.h>
#include <mach/mach.h>
#include <pthread/qos.h>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <vector>

namespace {

constexpr uint64_t kStack = 0x5357495443480005ull;

class Kernel {
public:
    uint32_t flags = TegraGM20BCompilerCapable | TegraGM20BCompilerRegistered | TegraGM20BCompilerStore;
    int called = kIOReturnSuccess, asyncCalled = kIOReturnSuccess, infoCalled = kIOReturnSuccess;
    unsigned infos = 0;
    int32_t answer = TegraGM20BCompileMade;
    uint32_t generation = 1, how = TegraGM20BCompileCompiled | 4u << 8;
    TegraGM20BAbiCompileTimes times = {};  // the compiler's, as its Complete gave them
    unsigned maps = 0, compiles = 0, asyncAsked = 0;
    std::function<void()> compiling;  // what the compiler does before it answers
    switch_seed_pack::StoreWriter store;

    // An asynchronous request as the kernel keeps it: its cookie, the callback its reference names and the port its answer
    // goes to
    struct Waiting {
        uint64_t cookie;
        uint64_t reference[kOSAsyncRef64Count];
        uint32_t port = 0;
    };

    std::vector<Waiting> waiting;
    size_t bound = SIZE_MAX;  // asynchronous requests the connection takes before kIOReturnNoResources

    // The oldest waiting request finished: `result` the call's (kIOReturnSuccess, or kIOReturnAborted for one ended
    // without an answer), `answer` the compiler's
    void deliver(int result, int32_t answer)
    {
        const Waiting w = waiting.front();
        waiting.erase(waiting.begin());
        const auto callback = reinterpret_cast<IOAsyncCallback>(uintptr_t(w.reference[kIOAsyncCalloutFuncIndex]));
        uint64_t words[TegraGM20BCompileAnswerWords] = {
            w.cookie,
            uint64_t(uint32_t(answer)) | uint64_t(generation) << 32,
            times.compile_start,
            times.compile_end,
            times.answered,
            uint64_t(times.answer_priority) | uint64_t(times.reserved) << 32};
        callback(reinterpret_cast<void *>(uintptr_t(w.reference[kIOAsyncCalloutRefconIndex])), result,
                 reinterpret_cast<void **>(words), TegraGM20BCompileAnswerWords);
    }

    // The oldest waiting request answered as the kernel answers it: IOUserClient's completion message to the request's own
    // port, which carries no class
    void send(int32_t answer)
    {
        const Waiting w = waiting.front();
        waiting.erase(waiting.begin());
        uint64_t words[TegraGM20BCompileAnswerWords] = {w.cookie, uint64_t(uint32_t(answer)) | uint64_t(generation)
                                                                                                   << 32};
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

    raw::Remote::Call call()
    {
        return [this](uint32_t method, const void *in, size_t, void *out, size_t outSize, uint64_t *reference,
                      uint32_t port) -> int {
            if (method == TegraGM20BMethodCompile && reference) {
                if (waiting.size() >= bound)
                    return kIOReturnNoResources;
                asyncAsked++;
                if (asyncCalled != kIOReturnSuccess)
                    return asyncCalled;
                compiles++;
                Waiting w = {static_cast<const TegraGM20BAbiCompile *>(in)->cookie, {}, port};
                memcpy(w.reference, reference, sizeof w.reference);
                waiting.push_back(w);
                return kIOReturnSuccess;
            }
            switch (method) {
            case TegraGM20BMethodCompilerInfo:
                infos++;
                if (infoCalled != kIOReturnSuccess)
                    return infoCalled;
                *static_cast<TegraGM20BAbiCompilerInfoReply *>(out) = {store.mappedBytes(), kStack, flags, generation};
                return kIOReturnSuccess;
            case TegraGM20BMethodMap:
                maps++;
                *static_cast<TegraGM20BAbiMapReply *>(out) = {uint64_t(uintptr_t(store.base())), store.mappedBytes()};
                return kIOReturnSuccess;
            case TegraGM20BMethodCompile:
                compiles++;
                if (outSize != sizeof(TegraGM20BAbiCompileReply))
                    return kIOReturnBadArgument;
                if (compiling)
                    compiling();
                *static_cast<TegraGM20BAbiCompileReply *>(out) = {answer, generation, how, 0, times};
                return called;
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
    raw::Digest name{};

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
    raw::Remote remote(kernel.call(), kStack);
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
    entry.resize(raw::programSlotEntryBytes(slot.size()));
    publish();
    const raw::Digest slotName = raw::programSlotName(slot.data(), slot.size());
    kernel.store.put(slotName.data(), entry.data(), uint32_t(entry.size()));
    raw::Remote remote(kernel.call(), kStack);
    std::string bytes;
    ASSERT_TRUE(remote.find(name, bytes)) << "a record lookup maps the store";
    const uint8_t *mapping = nullptr;
    uint64_t mapped = 0;
    const uint8_t *at = remote.findSlot(slot.data(), slot.size(), mapping, mapped);
    ASSERT_NE(at, nullptr);
    EXPECT_GE(at, mapping);
    EXPECT_LE(at + entry.size(), mapping + mapped);
    EXPECT_EQ(uint64_t(at - mapping) % raw::kProgramSlotPage, 0u);
    EXPECT_EQ(memcmp(at, slot.data(), slot.size()), 0);
    slot[100] ^= 1;
    EXPECT_EQ(remote.findSlot(slot.data(), slot.size(), mapping, mapped), nullptr) << "other bytes are another slot";
}

TEST_F(RemoteCompile, ARecordTheCompilerMadeIsReadOnceItAnswersFromItsNewGeneration)
{
    raw::Remote remote(kernel.call(), kStack);
    std::string bytes;
    EXPECT_FALSE(remote.find(name, bytes));
    kernel.compiling = [&] {
        publish();
        kernel.generation = 2;
    };
    raw::Remote::Answered answered;
    EXPECT_EQ(remote.compile("a request", &answered), raw::Remote::Answer::Made);
    EXPECT_EQ(answered.how, kernel.how);
    EXPECT_TRUE(remote.find(name, bytes));
    EXPECT_EQ(kernel.maps, 2u);
    EXPECT_TRUE(remote.find(name, bytes));
    EXPECT_EQ(kernel.maps, 2u);
}

TEST_F(RemoteCompile, ARequestWaitsForACompilerOnItsWay)
{
    kernel.flags = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected;
    raw::Remote remote(kernel.call(), kStack);
    EXPECT_TRUE(remote.serving());
    EXPECT_EQ(kernel.maps, 0u);
    EXPECT_EQ(remote.compile("a request"), raw::Remote::Answer::Made);
    EXPECT_EQ(kernel.compiles, 1u);
}

TEST_F(RemoteCompile, AProcessServedNoCompilerIsToldSoWithNoCall)
{
    kernel.flags = TegraGM20BCompilerCapable;
    raw::Remote remote(kernel.call(), kStack);
    EXPECT_FALSE(remote.serving());
    EXPECT_EQ(remote.compile("a request"), raw::Remote::Answer::Unavailable);
    EXPECT_FALSE(remote.compileAsync("a request", true, 0, [](raw::Remote::Answer, const raw::Remote::Answered &) {}));
    EXPECT_EQ(kernel.compiles, 0u);
    kernel.flags = 0;
    raw::Remote older(kernel.call(), kStack);
    EXPECT_FALSE(older.serving());
}

TEST_F(RemoteCompile, AProcessStartedBeforeTheCompilerIsServedOnceItRegisters)
{
    kernel.flags = TegraGM20BCompilerCapable;
    raw::Remote remote(kernel.call(), kStack);
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
    raw::Remote remote(kernel.call(), kStack);
    EXPECT_FALSE(remote.serving());
    kernel.infoCalled = kIOReturnSuccess;
    EXPECT_TRUE(remote.serving());
    std::string bytes;
    EXPECT_TRUE(remote.find(name, bytes));
    EXPECT_EQ(remote.compile("a request"), raw::Remote::Answer::Made);
}

// A failed call fails its own ask: no count, no latch, each later ask calls afresh
TEST_F(RemoteCompile, EveryAskAfterAFailedCompilerInfoCallsAfresh)
{
    kernel.infoCalled = kIOReturnNotResponding;
    raw::Remote remote(kernel.call(), kStack);
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
        raw::Remote remote(kernel.call(), kStack);
        for (int i = 0; i < 4; ++i)
            EXPECT_FALSE(remote.serving());
        EXPECT_FALSE(remote.declined());
        EXPECT_EQ(kernel.infos, 1u) << std::hex << absent;
    }
    kernel.infos = 0;
    kernel.infoCalled = kIOReturnSuccess;
    kernel.flags = 0;
    raw::Remote older(kernel.call(), kStack);
    EXPECT_FALSE(older.serving());
    EXPECT_EQ(kernel.infos, 1u);
}

TEST_F(RemoteCompile, AnInterruptedWaitOrAFullQueueIsAFailureALaterAskRetries)
{
    raw::Remote remote(kernel.call(), kStack);
    kernel.called = kIOReturnAborted;
    EXPECT_EQ(remote.compile("a request"), raw::Remote::Answer::Failed);
    kernel.called = kIOReturnNoResources;
    EXPECT_EQ(remote.compile("a request"), raw::Remote::Answer::Failed);
    kernel.called = kIOReturnSuccess;
    kernel.answer = TegraGM20BCompileUnavailable;
    EXPECT_EQ(remote.compile("a request"), raw::Remote::Answer::Unavailable);
}

// The third compiler lost: none registered, none waited for. The process keeps reading the store and making the states
// it lacks off the asking thread, as for any request a compiler did not take, but sends no request until one registers.
TEST_F(RemoteCompile, AProcessSendsNoRequestOnceOneFindsNoCompilerUntilOneRegisters)
{
    raw::Remote remote(kernel.call(), kStack);
    ASSERT_TRUE(remote.serving());
    kernel.flags = TegraGM20BCompilerCapable | TegraGM20BCompilerStore;
    kernel.answer = TegraGM20BCompileUnavailable;
    EXPECT_EQ(remote.compile("a request"), raw::Remote::Answer::Unavailable);
    EXPECT_TRUE(remote.serving());
    EXPECT_FALSE(remote.taking());
    EXPECT_EQ(remote.compile("a request"), raw::Remote::Answer::Unavailable);
    EXPECT_FALSE(remote.compileAsync("a request", true, 0, [](raw::Remote::Answer, const raw::Remote::Answered &) {}));
    EXPECT_EQ(kernel.compiles, 1u);
    publish();
    std::string bytes;
    EXPECT_TRUE(remote.find(name, bytes));
    kernel.flags = TegraGM20BCompilerCapable | TegraGM20BCompilerRegistered | TegraGM20BCompilerStore;
    kernel.answer = TegraGM20BCompileMade;
    EXPECT_TRUE(remote.taking());
    EXPECT_EQ(remote.compile("a request"), raw::Remote::Answer::Made);
    EXPECT_EQ(kernel.compiles, 2u);
}

TEST_F(RemoteCompile, AnAsynchronousRequestRefusedForWantOfACompilerStopsTheRequests)
{
    raw::Remote remote(kernel.call(), kStack);
    kernel.flags = TegraGM20BCompilerCapable;
    kernel.asyncCalled = kIOReturnNotReady;
    EXPECT_FALSE(remote.compileAsync("a request", true, 0, [](raw::Remote::Answer, const raw::Remote::Answered &) {}));
    EXPECT_TRUE(remote.serving());
    EXPECT_FALSE(remote.taking());
    EXPECT_FALSE(remote.compileAsync("a request", true, 0, [](raw::Remote::Answer, const raw::Remote::Answered &) {}));
    EXPECT_EQ(kernel.compiles, 0u);
    EXPECT_EQ(kernel.asyncAsked, 1u);
}

// A compiler that went with requests waiting answers them Unavailable: while the next one is on its way the process asks
// again at once
TEST_F(RemoteCompile, AnUnavailableAnswerWhileTheNextCompilerIsWaitedForKeepsTheRequests)
{
    raw::Remote remote(kernel.call(), kStack);
    kernel.flags = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected;
    kernel.answer = TegraGM20BCompileUnavailable;
    EXPECT_EQ(remote.compile("a request"), raw::Remote::Answer::Unavailable);
    EXPECT_TRUE(remote.taking());
    EXPECT_EQ(remote.compile("a request"), raw::Remote::Answer::Unavailable);
    EXPECT_EQ(kernel.compiles, 2u);
}

TEST_F(RemoteCompile, AStoreOfAnotherStackIsNotRead)
{
    open(kStack + 1);
    publish();
    raw::Remote remote(kernel.call(), kStack);
    std::string bytes;
    EXPECT_FALSE(remote.find(name, bytes));
}

// The answers callbacks got, in the order they came
struct Answers {
    std::vector<raw::Remote::Answer> got;
    std::vector<raw::Remote::Answered> with;

    raw::Remote::Done next()
    {
        return [this](raw::Remote::Answer a, const raw::Remote::Answered &answered) {
            got.push_back(a);
            with.push_back(answered);
        };
    }
};

TEST_F(RemoteCompile, AnAsynchronousAnswerReachesItsCallbackWithItsGeneration)
{
    raw::Remote remote(kernel.call(), kStack);
    Answers answers;
    ASSERT_TRUE(remote.compileAsync("a request", true, 0x21, answers.next()));
    EXPECT_TRUE(answers.got.empty());
    publish();
    kernel.generation = 2;
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    ASSERT_EQ(answers.got.size(), 1u);
    EXPECT_EQ(answers.got[0], raw::Remote::Answer::Made);
    std::string bytes;
    EXPECT_TRUE(remote.find(name, bytes));
    EXPECT_EQ(kernel.maps, 2u);
}

TEST_F(RemoteCompile, TwoRequestsTheCompilerAnswersFromOneCompileBothReadIt)
{
    raw::Remote remote(kernel.call(), kStack);
    Answers first, second;
    ASSERT_TRUE(remote.compileAsync("a request", true, 0x21, first.next()));
    ASSERT_TRUE(remote.compileAsync("a request", true, 0x19, second.next()));
    EXPECT_EQ(kernel.compiles, 2u);
    publish();
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    ASSERT_EQ(first.got.size(), 1u);
    ASSERT_EQ(second.got.size(), 1u);
    EXPECT_EQ(first.got[0], raw::Remote::Answer::Made);
    EXPECT_EQ(second.got[0], raw::Remote::Answer::Made);
    std::string bytes;
    EXPECT_TRUE(remote.find(name, bytes));
}

TEST_F(RemoteCompile, ARequestWhoseCompilerWentEndsUnavailableOrFailed)
{
    raw::Remote remote(kernel.call(), kStack);
    Answers lost, declined, twice;
    ASSERT_TRUE(remote.compileAsync("a request", true, 0x21, lost.next()));
    ASSERT_TRUE(remote.compileAsync("another", true, 0x21, declined.next()));
    ASSERT_TRUE(remote.compileAsync("a third", true, 0x21, twice.next()));
    kernel.deliver(kIOReturnAborted, 0);
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileUnavailable);
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileFailed);
    EXPECT_EQ(lost.got, std::vector<raw::Remote::Answer>{raw::Remote::Answer::Unavailable});
    EXPECT_EQ(declined.got, std::vector<raw::Remote::Answer>{raw::Remote::Answer::Unavailable});
    EXPECT_EQ(twice.got, std::vector<raw::Remote::Answer>{raw::Remote::Answer::Failed});
}

TEST_F(RemoteCompile, ARequestPastTheBoundIsSentOnceAnEarlierOneIsAnswered)
{
    kernel.bound = 1;
    raw::Remote remote(kernel.call(), kStack);
    Answers first, held;
    ASSERT_TRUE(remote.compileAsync("a request", true, 0x21, first.next()));
    ASSERT_TRUE(remote.compileAsync("another", true, 0x21, held.next()));
    EXPECT_EQ(kernel.waiting.size(), 1u);
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileMade);
    EXPECT_EQ(first.got.size(), 1u);
    ASSERT_EQ(kernel.waiting.size(), 1u);
    EXPECT_TRUE(held.got.empty());
    kernel.deliver(kIOReturnSuccess, TegraGM20BCompileRefused);
    EXPECT_EQ(held.got, std::vector<raw::Remote::Answer>{raw::Remote::Answer::Refused});
}

TEST_F(RemoteCompile, ARemoteThatGoesEndsWhatItWaitsFor)
{
    Answers waiting;
    {
        raw::Remote remote(kernel.call(), kStack);
        ASSERT_TRUE(remote.compileAsync("a request", true, 0x21, waiting.next()));
    }
    EXPECT_EQ(waiting.got, std::vector<raw::Remote::Answer>{raw::Remote::Answer::Unavailable});
}

// A debug compiler's account of the wait reaches the asker whole, answered either way, and splits the asker's wait
TEST_F(RemoteCompile, TheCompilersTimesReachTheAskerAndSplitItsWait)
{
    kernel.times = {2'000'000, 47'000'000, 48'000'000, 37, 0};
    publish();
    raw::Remote remote(kernel.call(), kStack);
    raw::Remote::Answered answered;
    ASSERT_EQ(remote.compile("a request", &answered), raw::Remote::Answer::Made);
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

    raw::Obtained o;
    o.split(1'000, 2'000'000, 47'000'000, 48'000'000, 48'500'000, 49'000);
    EXPECT_EQ(o.queueUs, 1'000);
    EXPECT_EQ(o.compileUs, 45'000);
    EXPECT_EQ(o.answerUs, 1'000);
    EXPECT_EQ(o.wakeUs, 500);
    EXPECT_EQ(o.storeUs, 500);
    raw::Obtained stored;
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
    raw::Remote remote(kernel.call(), kStack);

    struct Taken {
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        qos_class_t at = QOS_CLASS_UNSPECIFIED;
    };

    const qos_class_t asked[] = {QOS_CLASS_USER_INTERACTIVE, QOS_CLASS_BACKGROUND, QOS_CLASS_UNSPECIFIED,
                                 QOS_CLASS_UTILITY};
    Taken taken[std::size(asked)];
    for (size_t i = 0; i < std::size(asked); ++i)
        ASSERT_TRUE(remote.compileAsync("a request", true, asked[i],
                                        [&taken, i](raw::Remote::Answer, const raw::Remote::Answered &) {
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
        EXPECT_EQ(taken[i].at, raw::answerClass(asked[i])) << "answer " << i;
    }
}

// Completion's queue (RawComplete.mm) runs each block at its asker's class, below its own default as well as above it,
// and in order
TEST(AnswerQueue, EachBlockRunsAtItsAskersClassInOrder)
{
    dispatch_queue_t queue = raw::answerQueue("answer-queue-test");
    const qos_class_t asked[] = {QOS_CLASS_BACKGROUND, QOS_CLASS_USER_INTERACTIVE, QOS_CLASS_UTILITY,
                                 QOS_CLASS_UNSPECIFIED};
    std::vector<qos_class_t> ran;
    std::vector<qos_class_t> *into = &ran;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    for (qos_class_t qos : asked)
        raw::asyncAt(queue, qos, ^{
            into->push_back(qos_class_self());
            dispatch_semaphore_signal(done);
        });
    for (size_t i = 0; i < std::size(asked); ++i)
        ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))), 0);
    ASSERT_EQ(ran.size(), std::size(asked));
    for (size_t i = 0; i < std::size(asked); ++i)
        EXPECT_EQ(ran[i], raw::answerClass(asked[i])) << "block " << i;
    dispatch_release(queue);
}

}  // namespace
