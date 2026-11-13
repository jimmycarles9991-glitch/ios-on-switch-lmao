// SPDX-License-Identifier: MIT
// raw/runtime/raw_remote.h over a model of the kernel's compiler methods (TegraGM20BCompilerABI.h): the store mapped
// once and read with no call; a state the store lacks asked of the compiler, its record read once it answers (the store
// mapped again for a new generation); a request waits for a compiler on its way, and a process the kernel serves no
// compiler is told so without a call. Asked asynchronously: each answer reaches its own callback (two requests the
// compiler answers from one compile, a compiler lost while they wait, an Unavailable end), a request past the
// connection's bound is held until an earlier one is answered, and a remote that goes ends what it still waits for.
#include "../raw/runtime/raw_remote.h"
#include "../raw/runtime/raw_cache_io.h"
#include "../raw/runtime/raw_program_pages.h"
#include "TegraGM20BABI.h"
#include "TegraGM20BCompilerABI.h"

#include <gtest/gtest.h>

#include <IOKit/IOReturn.h>
#include <IOKit/IOKitLib.h>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <vector>

namespace {

constexpr uint64_t kStack = 0x5357495443480005ull;

class Kernel {
public:
    uint32_t flags = TegraGM20BCompilerCapable | TegraGM20BCompilerRegistered | TegraGM20BCompilerStore;
    int called = kIOReturnSuccess, asyncCalled = kIOReturnSuccess;
    int32_t answer = TegraGM20BCompileMade;
    uint32_t generation = 1, how = TegraGM20BCompileCompiled | 4u << 8;
    unsigned maps = 0, compiles = 0, asyncAsked = 0;
    std::function<void()> compiling;  // what the compiler does before it answers
    switch_seed_pack::StoreWriter store;

    // An asynchronous request as the kernel keeps it: its cookie and the callback its reference names
    struct Waiting {
        uint64_t cookie;
        uint64_t reference[kOSAsyncRef64Count];
    };

    std::vector<Waiting> waiting;
    size_t bound = SIZE_MAX;  // asynchronous requests the connection takes before kIOReturnNoResources

    // The oldest waiting request finished: `result` the call's (kIOReturnSuccess, or kIOReturnAborted for one ended
    // without an answer), `answer` the compiler's
    void deliver(int result, int32_t answer)
    {
        const Waiting w = waiting.front();
        waiting.erase(waiting.begin());
        using Callback = void (*)(void *, int, void *, void *);
        const auto callback = reinterpret_cast<Callback>(uintptr_t(w.reference[kIOAsyncCalloutFuncIndex]));
        const uint64_t word = uint64_t(uint32_t(answer)) | uint64_t(generation) << 32;
        callback(reinterpret_cast<void *>(uintptr_t(w.reference[kIOAsyncCalloutRefconIndex])), result,
                 reinterpret_cast<void *>(uintptr_t(w.cookie)), reinterpret_cast<void *>(uintptr_t(word)));
    }

    raw::Remote::Call call()
    {
        return [this](uint32_t method, const void *in, size_t, void *out, size_t outSize, uint64_t *reference,
                      uint32_t) -> int {
            if (method == TegraGM20BMethodCompile && reference) {
                if (waiting.size() >= bound)
                    return kIOReturnNoResources;
                asyncAsked++;
                if (asyncCalled != kIOReturnSuccess)
                    return asyncCalled;
                compiles++;
                Waiting w = {static_cast<const TegraGM20BAbiCompile *>(in)->cookie, {}};
                memcpy(w.reference, reference, sizeof w.reference);
                waiting.push_back(w);
                return kIOReturnSuccess;
            }
            switch (method) {
            case TegraGM20BMethodCompilerInfo:
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
                *static_cast<TegraGM20BAbiCompileReply *>(out) = {answer, generation, how, 0};
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
    uint32_t how = 0;
    EXPECT_EQ(remote.compile("a request", &how), raw::Remote::Answer::Made);
    EXPECT_EQ(how, kernel.how);
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
    EXPECT_FALSE(remote.compileAsync("a request", true, 0, [](raw::Remote::Answer) {}));
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
    EXPECT_FALSE(remote.compileAsync("a request", true, 0, [](raw::Remote::Answer) {}));
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
    EXPECT_FALSE(remote.compileAsync("a request", true, 0, [](raw::Remote::Answer) {}));
    EXPECT_TRUE(remote.serving());
    EXPECT_FALSE(remote.taking());
    EXPECT_FALSE(remote.compileAsync("a request", true, 0, [](raw::Remote::Answer) {}));
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

    std::function<void(raw::Remote::Answer)> next()
    {
        return [this](raw::Remote::Answer a) {
            got.push_back(a);
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

TEST(InProcess, ItsReasonIsLoggedOnceAndEveryCompileCounted)
{
    raw::InProcess tally;
    EXPECT_TRUE(tally.compiled(raw::InProcess::NoCompiler));
    EXPECT_FALSE(tally.compiled(raw::InProcess::NoCompiler));
    EXPECT_EQ(tally.count(), 2u);
}

}  // namespace
