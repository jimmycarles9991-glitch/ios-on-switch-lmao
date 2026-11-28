// SPDX-License-Identifier: MIT
// IOSurface imports on a client's gm20b::Device over the kernel model, with one surface's BoImport held where it arrives in
// the model (kernel_model::arriving): the device lock is not held across the import's kernel calls, so the submit worker's
// CPU completion capture and another surface's import go on meanwhile; a second import of the held surface waits for it
// and shares it (one BoImport, one layout ask), and an import that fails publishes nothing and lets that second one make
// its own; a no-copy buffer over the held surface's pages waits for its layout. Plain and thread-sanitized.
#include "kernel_model.h"
#include "empty_pack.h"
#include "../gpu.h"
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr uint32_t kHeldSurface = 7, kBGRA8Unorm = 80, kSide = 64, kPitch = kSide * 4;
constexpr uint64_t kBytes = 16384;

// The held surface's first BoImport, held on its thread until release() answers it; the threads it started are let go and
// joined at its end
class HeldImport {
public:
    HeldImport()
    {
        held = this;
        kernel_model::arriving.store(&arrive, std::memory_order_release);
    }

    ~HeldImport()
    {
        release(KERN_SUCCESS);
        join();
        kernel_model::arriving.store(nullptr, std::memory_order_release);
        held = nullptr;
    }

    void start(std::function<void()> work)
    {
        threads_.emplace_back(std::move(work));
    }

    bool reached()
    {
        std::unique_lock<std::mutex> l(lock_);
        return changed_.wait_for(l, 5s, [&] {
            return holding_;
        });
    }

    void release(int answer)
    {
        std::lock_guard<std::mutex> l(lock_);
        if (!released_)
            answer_ = answer;
        released_ = true;
        changed_.notify_all();
    }

    void join()
    {
        for (std::thread &t : threads_)
            t.join();
        threads_.clear();
    }

    // The held surface's BoImports so far
    unsigned imports()
    {
        std::lock_guard<std::mutex> l(lock_);
        return imports_;
    }

private:
    static int arrive(uint32_t method, const void *input)
    {
        if (method != TegraGM20BMethodBoImport ||
            static_cast<const TegraGM20BAbiBoImport *>(input)->surface_id != kHeldSurface)
            return KERN_SUCCESS;
        HeldImport &h = *held;
        std::unique_lock<std::mutex> l(h.lock_);
        if (h.imports_++)
            return KERN_SUCCESS;
        h.holding_ = true;
        h.changed_.notify_all();
        h.changed_.wait(l, [&] {
            return h.released_;
        });
        return h.answer_;
    }

    static inline HeldImport *held = nullptr;
    std::vector<std::thread> threads_;
    std::mutex lock_;
    std::condition_variable changed_;
    unsigned imports_ = 0;
    bool holding_ = false, released_ = false;
    int answer_ = KERN_SUCCESS;
};

class GM20BDeviceImport : public testing::Test {
protected:
    gm20b::Device dev;
    void *pages[2] = {aligned_alloc(kBytes, kBytes), aligned_alloc(kBytes, kBytes)};

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.infoFlags = TegraGM20BInfoSurfaceLayout;
        std::string err;
        ASSERT_TRUE(dev.open(emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    }

    ~GM20BDeviceImport() override
    {
        for (void *p : pages)
            free(p);
    }

    bool surfaceTexture(uint64_t id, void *base, gm20b::Texture &t)
    {
        return dev.createSurfaceTexture(id, base, kBytes, nullptr, 0, kSide, kSide, kPitch, kBGRA8Unorm, t, true);
    }

    unsigned liveImports(void *base)
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        unsigned n = 0;
        for (const auto &[handle, bo] : kernel_model::model.bos)
            n += bo.import && bo.address == uint64_t(uintptr_t(base));
        return n;
    }
};

TEST_F(GM20BDeviceImport, TheSubmitWorkersCaptureAndAnotherImportDoNotWaitForAnImport)
{
    gm20b::Buffer written;
    ASSERT_TRUE(dev.createBuffer(4096, written));
    gm20b::Texture held, other;
    bool heldMade = false;
    {
        HeldImport hold;
        hold.start([&] {
            heldMade = surfaceTexture(kHeldSurface, pages[0], held);
        });
        ASSERT_TRUE(hold.reached());
        auto capture = std::async(std::launch::async, [&] {
            return dev.captureCpuCompletion(1, dev.channelGeneration(), {&written}).valid;
        });
        const bool captured = capture.wait_for(2s) == std::future_status::ready;
        auto import = std::async(std::launch::async, [&] {
            return surfaceTexture(kHeldSurface + 1, pages[1], other);
        });
        const bool imported = import.wait_for(2s) == std::future_status::ready;
        hold.release(KERN_SUCCESS);
        hold.join();
        EXPECT_TRUE(captured) << "the CPU completion capture waited for the other thread's import";
        EXPECT_TRUE(imported) << "the import of another surface waited for the other thread's import";
        (void)capture.get();
        EXPECT_TRUE(import.get());
    }
    EXPECT_TRUE(heldMade);
    dev.destroyTexture(held);
    dev.destroyTexture(other);
    dev.destroyBuffer(written);
}

// A guard (it passes with the device lock held across the kernel calls too): the second import of a surface the first is
// still importing takes the first's import, with no BoImport of its own
TEST_F(GM20BDeviceImport, TwoThreadsImportingOneSurfaceShareOneImport)
{
    gm20b::Texture first, second;
    bool firstMade = false, secondMade = false;
    unsigned imports = 0;
    {
        HeldImport hold;
        hold.start([&] {
            firstMade = surfaceTexture(kHeldSurface, pages[0], first);
        });
        ASSERT_TRUE(hold.reached());
        hold.start([&] {
            secondMade = surfaceTexture(kHeldSurface, pages[0], second);
        });
        std::this_thread::sleep_for(50ms);
        hold.release(KERN_SUCCESS);
        hold.join();
        imports = hold.imports();
    }
    ASSERT_TRUE(firstMade && secondMade);
    EXPECT_EQ(imports, 1u);
    EXPECT_EQ(first.import, second.import);
    EXPECT_EQ(first.import->refs, 2u);
    EXPECT_EQ(liveImports(pages[0]), 1u);
    dev.destroyTexture(first);
    dev.destroyTexture(second);
    EXPECT_EQ(liveImports(pages[0]), 0u);
}

// The second thread's wait for the first's import sleeps on a word the importing thread owns, so the importer runs at
// the waiter's priority while it binds (M19: a condition variable lent nothing)
TEST_F(GM20BDeviceImport, AThreadWaitingForAnImportNamesTheImporter)
{
    gm20b::Texture first, second;
    std::atomic<uint32_t> importer{0};
    {
        HeldImport hold;
        hold.start([&] {
            importer = gm20b::OwnedWord::currentOwner();
            surfaceTexture(kHeldSurface, pages[0], first);
        });
        ASSERT_TRUE(hold.reached());
        EXPECT_EQ(dev.surfaceImporter(kHeldSurface, pages[0], kBytes), importer.load());
        hold.start([&] {
            surfaceTexture(kHeldSurface, pages[0], second);
        });
        std::this_thread::sleep_for(50ms);
        EXPECT_EQ(dev.surfaceImporter(kHeldSurface, pages[0], kBytes), importer.load())
            << "the waiter leaves the importer's word as it is";
        hold.release(KERN_SUCCESS);
        hold.join();
    }
    EXPECT_EQ(dev.surfaceImporter(kHeldSurface, pages[0], kBytes), 0u);
    EXPECT_EQ(first.import, second.import);
    dev.destroyTexture(first);
    dev.destroyTexture(second);
}

// A guard as above: the first import's BoImport is refused; the second import, which waited for it, finds nothing published
// and makes its own
TEST_F(GM20BDeviceImport, AFailedImportPublishesNothingAndTheWaitingImportMakesItsOwn)
{
    gm20b::Texture first, second;
    bool firstMade = true, secondMade = false;
    unsigned imports = 0;
    {
        HeldImport hold;
        hold.start([&] {
            firstMade = surfaceTexture(kHeldSurface, pages[0], first);
        });
        ASSERT_TRUE(hold.reached());
        hold.start([&] {
            secondMade = surfaceTexture(kHeldSurface, pages[0], second);
        });
        std::this_thread::sleep_for(50ms);
        hold.release(kIOReturnNoResources);
        hold.join();
        imports = hold.imports();
    }
    EXPECT_FALSE(firstMade);
    ASSERT_TRUE(secondMade);
    EXPECT_EQ(imports, 2u);
    EXPECT_EQ(second.import->refs, 1u);
    EXPECT_EQ(liveImports(pages[0]), 1u);
    dev.destroyTexture(second);
    EXPECT_EQ(liveImports(pages[0]), 0u);
}

// A no-copy buffer over pages of a surface another thread is importing waits for that import: the kernel lays the
// surface out block-linear at its first ask, and a buffer over its GOBs would read and write them as rows, so none is made
TEST_F(GM20BDeviceImport, ANoCopyBufferOverASurfaceBeingImportedWaitsForItsLayout)
{
    kernel_model::model.surfaceLayout = TegraGM20BLayoutTiled;
    CFDataRef owner = CFDataCreate(kCFAllocatorDefault, nullptr, 0);
    gm20b::Texture held;
    gm20b::Buffer over;
    bool heldMade = false, made = true, tiled = false, waited = false;
    {
        HeldImport hold;
        hold.start([&] {
            heldMade = dev.createSurfaceTexture(kHeldSurface, pages[0], kBytes, owner, 0, kSide, kSide, kPitch,
                                                kBGRA8Unorm, held, true);
        });
        ASSERT_TRUE(hold.reached());
        auto buffer = std::async(std::launch::async, [&] {
            return dev.createBufferNoCopy(static_cast<uint8_t *>(pages[0]) + 4096, 4096, over, tiled);
        });
        waited = buffer.wait_for(200ms) == std::future_status::timeout;
        hold.release(KERN_SUCCESS);
        hold.join();
        made = buffer.get();
    }
    EXPECT_TRUE(waited) << "the no-copy buffer was made beside the surface's import";
    EXPECT_FALSE(made);
    EXPECT_TRUE(tiled);
    if (heldMade)
        dev.destroyTexture(held);
    if (made)
        dev.destroyBuffer(over);
    CFRelease(owner);
}

}  // namespace
