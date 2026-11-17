// SPDX-License-Identifier: MIT
// The system shader compiler (drivers/TegraGM20B/TegraGM20BCompilerABI.h) as a GPU client reaches it, over the process's
// own TegraGM20B connection: its store mapped read only (once, and again only when a compiler registered another), a lookup
// there with no system call, and a request answered either on the calling thread, which sleeps in the kernel until the
// compiler answers (a state Metal makes at once: the compiler runs it at that thread's class), or on this remote's queue
// (one Metal lets finish later, and work nobody waits for). A request that finds no compiler waits for one, unless the
// kernel says none will come this boot.
#pragma once
#include "raw_lock.h"
#include <IOKit/IOReturn.h>
#include <pthread/qos.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "../../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "raw_compile_request.h"
#include "TegraGM20BCompilerABI.h"
#include "raw_kernel_transport.h"

namespace raw {

// The kernel's CompilerInfo call as a client takes it. A kernel without the compiler's methods answers
// kIOReturnUnsupported, a process with no connection never gets one: neither changes in the process's life. Any other
// failed call fails that ask alone (one error line), and the next ask calls afresh.
enum class CompilerMethods : uint8_t {
    Unknown,
    Present,
    Absent
};

constexpr CompilerMethods compilerMethods(int called, uint32_t flags)
{
    if (called == kIOReturnSuccess)
        return (flags & TegraGM20BCompilerCapable) ? CompilerMethods::Present : CompilerMethods::Absent;
    return called == kIOReturnUnsupported || called == kIOReturnNotOpen ? CompilerMethods::Absent
                                                                        : CompilerMethods::Unknown;
}

class Remote {
public:
    enum class Answer : uint8_t {
        Made,
        Refused,
        Failed,      // the compile could not run: a later ask may
        Unavailable  // no compiler this boot, or it did not take the request
    };

    // What came with an answer: how the compiler answered and the class it ran at (TegraGM20BCompilerABI.h's `how`), a
    // debug compiler's account of the wait, and (debug builds) when this process had the answer, in CLOCK_UPTIME_RAW ns
    struct Answered {
        uint32_t how = 0;
        TegraGM20BAbiCompileTimes times = {};
        uint64_t at = 0;
    };

    using Done = std::function<void(Answer, const Answered &)>;

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
    // Asked of the compiler at the calling thread's class; the thread sleeps until it answers
    Answer compile(const std::string &request, Answered *answered = nullptr);
    // Asked with `done` called at the asker's class (raw_answer_class.h): `waited`, at class `qos` (the waiting thread's),
    // else as background work. A request past the kernel's bound on this connection waits here for one of them to be
    // answered. False: not asked (`done` is not called)
    bool compileAsync(const std::string &request, bool waited, uint32_t qos, Done done);

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
        Done done;
    };

    const Call call_;
    const uint64_t stack_;
    std::atomic<CompilerMethods> methods_{CompilerMethods::Unknown};
    std::atomic<bool> expected_{false}, declined_{false};
    std::atomic<bool> absent_{false};
    std::atomic<const Mapping *> mapping_{nullptr};
    std::atomic<uint32_t> latest_{0};  // the newest generation a completion named
    raw::Mutex mapLock_, asyncLock_;
    std::vector<const Mapping *> mappings_;  // every one ever made: a reader may still be inside an older one

    // One notification port per class an answer is taken at, each with a queue of that class: the kernel's answer
    // carries no class, so the port it is sent to sets it
    struct Port {
        void *port = nullptr, *queue = nullptr;
    };

    static constexpr qos_class_t kAnswerClasses[] = {QOS_CLASS_USER_INTERACTIVE, QOS_CLASS_USER_INITIATED,
                                                     QOS_CLASS_DEFAULT, QOS_CLASS_UTILITY, QOS_CLASS_BACKGROUND};
    std::array<Port, std::size(kAnswerClasses)> ports_;  // asyncLock_'s
    std::map<uint64_t, Done> pending_;
    std::vector<Held> held_;  // past the bound, asked once an earlier one is answered
    uint64_t nextCookie_ = 1;

    // asyncLock_ held: the request asked, or held past the bound; false: neither
    bool ask(Held &&held);
    bool capable();
    void refresh(const TegraGM20BAbiCompilerInfoReply &info);
    void recheck();
    int call(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize);
    // asyncLock_ held: the port answers taken at `cls` arrive on, made at its first ask; 0 for none
    uint32_t portAt(qos_class_t cls);
    void map(uint32_t generation);
    static Call transportCall(const KernelTransport &transport);
    static Answer answerOf(int32_t result);
    static void delivered(void *refcon, int result, void **words, uint32_t count);
};

}  // namespace raw
