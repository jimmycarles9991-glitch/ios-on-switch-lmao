// SPDX-License-Identifier: MIT
// The system shader compiler (drivers/TegraGM20B/TegraGM20BCompilerABI.h) as a GPU client reaches it, over the process's
// own TegraGM20B connection: its store mapped read only (once, and again only when a compiler registered another), a lookup
// there with no system call, and a request answered either on the calling thread, which sleeps in the kernel until the
// compiler answers (a state Metal makes at once: the compiler runs it at that thread's class), or on this remote's queue
// (one Metal lets finish later, and work nobody waits for). A request that finds no compiler waits for one, unless the
// kernel says none will come this boot.
#pragma once
#include "raw_lock.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "../../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "raw_compile_request.h"
#include "TegraGM20BCompilerABI.h"
#include "raw_kernel_transport.h"

namespace raw {

class Remote {
public:
    enum class Answer : uint8_t {
        Made,
        Refused,
        Failed,      // the compile could not run: a later ask may
        Unavailable  // no compiler this boot, or it did not take the request
    };

    // One call on the connection: a struct method, or with `reference` its asynchronous form (raw_kernel.cpp's forms)
    using Call = std::function<int(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize,
                                   uint64_t *reference, uint32_t port)>;

    Remote(const KernelTransport &transport, uint64_t stack);
    Remote(Call kernel, uint64_t stack);
    ~Remote();
    Remote(const Remote &) = delete;
    Remote &operator=(const Remote &) = delete;

    // The kernel takes requests: it has the compiler's methods and a compiler is registered or on its way (asked of the
    // kernel again while it is not: a compiler may register later)
    bool serving();
    // A compiler may take a request now. False once a request found none registered and none waited for (a crash loop's
    // third loss, or a decline): from then on each ask is the kernel's CompilerInfo, never a request, until a compiler
    // registers.
    bool taking();
    // The boot has a compiler, but none comes: it declined, or its launcher stopped starting it (asked of the kernel)
    bool declined();

    // The store's entry under `name`, copied out of the mapping
    bool find(const Digest &name, std::string &bytes);
    // A program slot's bytes where the mapped store holds them (switch_compiler::Store::putProgramSlots), in place, with
    // that mapping's first byte and size in `mapping` and `bytes`; null where it holds none of them
    const uint8_t *findSlot(const uint8_t *slot, size_t size, const uint8_t *&mapping, uint64_t &bytes) const;
    // The mapped store's identity (its stack and the pack rules' digest) in hex, "" with none mapped
    std::string storeIdentity() const;
    // The mapped store's first byte and size; null with none mapped
    const uint8_t *storeMapping(uint64_t &bytes) const;
    // Asked of the compiler at the calling thread's class; the thread sleeps until it answers. *how: the reply's
    // (TegraGM20BCompilerABI.h TegraGM20BCompileCompiled... and the class it ran at)
    Answer compile(const std::string &request, uint32_t *how = nullptr);
    // Asked with `done` called on this remote's queue; `waited`: at class `qos` (the waiting thread's), else as background
    // work. A request past the kernel's bound on this connection waits here for one of them to be answered. False: not
    // asked (`done` is not called)
    bool compileAsync(const std::string &request, bool waited, uint32_t qos, std::function<void(Answer)> done);

private:
    struct Mapping {
        uint32_t generation = 0;
        const uint8_t *base = nullptr;
        uint64_t bytes = 0;
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        switch_seed_pack::StoreReader reader;
    };

    struct Held {
        std::string request;
        bool waited;
        uint32_t qos;
        std::function<void(Answer)> done;
    };

    const Call call_;
    const uint64_t stack_;
    bool capable_ = false;
    std::atomic<bool> expected_{false}, declined_{false};
    std::atomic<bool> absent_{false};
    std::atomic<const Mapping *> mapping_{nullptr};
    std::atomic<uint32_t> latest_{0};  // the newest generation a completion named
    raw::Mutex mapLock_, asyncLock_;
    std::vector<const Mapping *> mappings_;  // every one ever made: a reader may still be inside an older one
    void *port_ = nullptr, *queue_ = nullptr;
    std::map<uint64_t, std::function<void(Answer)>> pending_;
    std::vector<Held> held_;  // past the bound, asked once an earlier one is answered
    uint64_t nextCookie_ = 1;

    // asyncLock_ held: the request asked, or held past the bound; false: neither
    bool ask(Held &&held);
    void refresh(const TegraGM20BAbiCompilerInfoReply &info);
    void recheck();
    int call(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize, uint64_t *reference = nullptr);
    void map(uint32_t generation);
    static Call transportCall(const KernelTransport &transport);
    static Answer answerOf(int32_t result);
    static void delivered(void *refcon, int result, void *cookie, void *word);
};

// The states a process compiles itself, and why: each reason's first is the one to log. A process compiles none while a
// compiler serves the boot: a state no request can name, or one the compiler does not take or store, fails its creation.
class InProcess {
public:
    enum Why : uint32_t {
        NoCompiler = 1u << 0,  // the boot has none (switch-no-bt on iOS 17 hides its launcher)
    };

    // A state about to be compiled here for `why`: true the first time `why` is given
    bool compiled(Why why)
    {
        count_.fetch_add(1, std::memory_order_relaxed);
        return !(reasons_.fetch_or(why, std::memory_order_relaxed) & why);
    }

    uint64_t count() const
    {
        return count_.load(std::memory_order_relaxed);
    }

    static const char *text(Why why);

private:
    std::atomic<uint64_t> count_{0};
    std::atomic<uint32_t> reasons_{0};
};

}  // namespace raw
