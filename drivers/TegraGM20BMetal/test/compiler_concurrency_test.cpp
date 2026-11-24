// SPDX-License-Identifier: MIT
// The shader compiler's engine (compiler_engine.h) as the pool's workers run it: four threads compile through one
// Engine, let go together so they ask the same state at the same moment, then each in its own turn of the list so states
// that share a stage meet, a kernel's facts and its dispatches among them, all reading one library of a client's data.
// Every answer is byte for byte the one a single worker gives cold. Each cache's rule is held as well: the workers that
// ask one stage or one library at once get the very object the first of them made. compiler_concurrency_tsan_test runs
// the same thread-sanitized (the C++ units; m2v's kept translations and Mesa are not instrumented).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "compiler_stage_cases.h"
#include "../compiler/compiler_engine.h"
#include "../compiler/compiler_libraries.h"
#include "../compiler/compiler_record.h"
#include "../compiler/compiler_stages.h"
#include "../TegraGM20BMetalDirect.h"
#include "../TegraGM20BMetalOffline.h"
#include "../pack.h"

#include <gtest/gtest.h>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace {

constexpr unsigned kWorkers = 4;

// Holds every worker until all have come, so their first compiles start together
class Gate {
public:
    explicit Gate(unsigned count) : left_(count)
    {}

    void pass()
    {
        std::unique_lock<std::mutex> l(lock_);
        if (--left_ == 0)
            opened_.notify_all();
        opened_.wait(l, [this] {
            return left_ == 0;
        });
    }

private:
    std::mutex lock_;
    std::condition_variable opened_;
    unsigned left_;
};

void together(const std::function<void(unsigned)> &work)
{
    Gate gate(kWorkers);
    std::vector<std::thread> workers;
    for (unsigned w = 0; w < kWorkers; w++)
        workers.emplace_back([&gate, &work, w] {
            gate.pass();
            work(w);
        });
    for (std::thread &t : workers)
        t.join();
}

struct Answer {
    int32_t result = -1;
    std::string entry, why;
};

// States that share stages (the base, its RGBA8 and RGBA16Float colours, blending off, four samples, and the fetching
// state with its RGBA8 colour and blending on), then a kernel's facts and three of its dispatches
std::vector<switch_compiler::Prepared> requests()
{
    std::vector<gm20b::CompileRequest> asked;
    compiler_test::Requests made(root(), {{kLibrary, library()}});
    const std::vector<Case> cases = renderCases();
    for (const size_t i : {0, 1, 2, 5, 11, 17, 18, 19}) {
        gm20b::CompileRequest r;
        std::string why;
        EXPECT_TRUE(made.render(cases[i].line, r, why)) << cases[i].name << ": " << why;
        asked.push_back(r);
    }
    const std::string bytes = fileBytes(library());
    const uint32_t shapes[][3] = {{8, 8, 1}, {2, 8, 1}, {16, 16, 1}};
    asked.push_back(kernelRequest(bytes, gm20b::RequestKind::Facts, shapes[0], 0));
    for (const auto &shape : shapes)
        asked.push_back(kernelRequest(bytes, gm20b::RequestKind::Kernel, shape, 80));
    std::vector<switch_compiler::Prepared> out(asked.size());
    switch_compiler::SystemLibraries system(root());
    for (size_t i = 0; i < asked.size(); i++) {
        std::string why;
        EXPECT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(asked[i]), 0, system, out[i], why)) << why;
    }
    return out;
}

// An engine as the service opens it, its process keeping `keep` bytes of translations from none
std::unique_ptr<switch_compiler::Engine> engine(size_t keep)
{
    auto e = std::make_unique<switch_compiler::Engine>();
    std::string why;
    EXPECT_TRUE(e->open(0, {compiler_test::emptyPack(dir()), root(), {}}, why)) << why;
    nmb_keep_translations(0);
    nmb_keep_translations(keep);
    return e;
}

Answer compile(switch_compiler::Engine &e, const switch_compiler::Prepared &p)
{
    Answer a;
    a.result = e.compile(p, a.entry, a.why);
    return a;
}

// compiler_stages.h's memo with the stage each answer holds, by the thread that asked
struct Holding final : public gm20b::StageMemo {
    explicit Holding(size_t bytes) : kept(bytes)
    {}

    int32_t compile(const gm20b::StageCompile &c, const gm20b::NvkStageInfo &info, const gm20b::NakLayout &layout,
                    gm20b::NvkStage &out, std::shared_ptr<const void> &held) override
    {
        const int32_t result = kept.compile(c, info, layout, out, held);
        std::lock_guard<std::mutex> l(lock);
        holders[std::this_thread::get_id()].push_back(held.get());
        return result;
    }

    switch_compiler::CompiledStages kept;
    std::mutex lock;
    std::map<std::thread::id, std::vector<const void *>> holders;
};

class CompilerConcurrency : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (root().empty() || library().empty())
            GTEST_SKIP() << "no crawl or no test library";
    }

    void TearDown() override
    {
        nmb_keep_translations(0);
    }
};

}  // namespace

TEST_F(CompilerConcurrency, WorkersAskingAtOnceGetTheAnswersOneWorkerGivesCold)
{
    const std::vector<switch_compiler::Prepared> asked = requests();
    std::vector<Answer> cold;
    for (const switch_compiler::Prepared &p : asked)
        cold.push_back(compile(*engine(0), p));
    for (size_t i = 0; i < cold.size(); i++)
        ASSERT_EQ(cold[i].result, TegraGM20BCompileMade) << "request " << i << ": " << cold[i].why;
    for (const bool turned : {false, true}) {
        const std::unique_ptr<switch_compiler::Engine> e = engine(kKept);
        std::vector<std::vector<Answer>> got(kWorkers, std::vector<Answer>(asked.size()));
        together([&](unsigned w) {
            for (size_t n = 0; n < asked.size(); n++) {
                const size_t i = turned ? (n + 3 * w) % asked.size() : n;
                got[w][i] = compile(*e, asked[i]);
            }
        });
        for (unsigned w = 0; w < kWorkers; w++)
            for (size_t i = 0; i < asked.size(); i++) {
                EXPECT_EQ(got[w][i].result, cold[i].result) << got[w][i].why;
                EXPECT_EQ(got[w][i].entry, cold[i].entry)
                    << (turned ? "each in its turn" : "all at once") << ", worker " << w << ", request " << i
                    << ": not the answer one worker gives cold";
            }
    }
}

TEST_F(CompilerConcurrency, WorkersAskingOneStateAtOnceShareEachStageTheFirstMade)
{
    const std::vector<switch_compiler::Prepared> asked = requests();
    gm20b::Pack pack;
    std::string why;
    ASSERT_TRUE(pack.load(compiler_test::emptyPack(dir()).c_str(), 0, why)) << why;
    const auto cache = std::make_shared<switch_compiler::LibraryCache>(root());
    const switch_compiler::Libraries libraries = [&cache](const gm20b::RequestLibrary &l, std::string &failed) {
        return cache->library(l, failed);
    };
    // kept whole, and over a bound of about one state's stages, where the workers' states push each other's out
    for (const size_t bytes : {size_t(kKept), size_t(48) << 10}) {
        Holding holding(bytes);
        std::vector<Answer> got(kWorkers * 3);
        cache->began();
        together([&](unsigned w) {
            gm20b::Direct direct(gm20b::PackRules::of(pack), false, offline::translator());
            direct.keepStages(&holding);
            for (size_t n = 0; n < 3; n++) {
                Answer &a = got[w * 3 + n];
                const size_t i = bytes == kKept ? 0 : (n + w) % 3;
                a.result = switch_compiler::makeEntry(asked[i], libraries, direct, 0, a.entry, a.why);
            }
        });
        cache->ended();
        for (unsigned w = 0; w < kWorkers; w++)
            for (size_t n = 0; n < 3; n++) {
                const size_t i = bytes == kKept ? 0 : (n + w) % 3;
                const Answer &a = got[w * 3 + n], cold = compile(*engine(0), asked[i]);
                EXPECT_EQ(a.result, TegraGM20BCompileMade) << a.why;
                EXPECT_EQ(a.entry, cold.entry) << bytes << " bytes kept, worker " << w << ", request " << i;
            }
        if (bytes != kKept)
            continue;
        ASSERT_EQ(holding.holders.size(), size_t(kWorkers));
        const std::vector<const void *> &first = holding.holders.begin()->second;
        for (const auto &[worker, held] : holding.holders)
            EXPECT_EQ(held, first) << "a worker compiled a stage another asked at the same time";
    }
}

TEST_F(CompilerConcurrency, WorkersAskingOneLibraryAtOnceReadItOnce)
{
    const auto cache = std::make_shared<switch_compiler::LibraryCache>(root());
    gm20b::RequestLibrary system;
    system.path = compiler_test::kQuartzCore;
    system.system = true;
    gm20b::RequestLibrary data;
    data.path = kLibrary;
    data.bytes = fileBytes(library());
    std::vector<id<MTLLibrary>> systems(kWorkers), datas(kWorkers);
    std::vector<const void *> handles(kWorkers);
    cache->began();
    together([&](unsigned w) {
        std::string why;
        datas[w] = cache->library(data, why);
        systems[w] = cache->library(system, why);
        handles[w] = offline::libraryHandle(systems[w]);
    });
    cache->ended();
    for (unsigned w = 0; w < kWorkers; w++) {
        ASSERT_TRUE(systems[w] && datas[w]);
        EXPECT_EQ(systems[w], systems[0]) << "worker " << w << " read the system library again";
        EXPECT_EQ(datas[w], datas[0]) << "worker " << w << " read the data library again";
        EXPECT_EQ(handles[w], handles[0]) << "worker " << w << " holds another translator library";
    }
}
