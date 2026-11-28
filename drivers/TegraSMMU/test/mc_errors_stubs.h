// Host stand-ins for what TegraMCErrors.cpp takes from the kernel in mc_errors_test: IOKit's filter interrupt event
// source (it hands the filter and action to the test), a work loop, the GIC nub, IOLog into a list, and the spin lock.
// While switch_test_interrupts_off is set (a filter's context), IOLog, a lock, a sleep or an allocation throws.
#ifndef SWITCH_TEST_MC_ERRORS_STUBS_H
#define SWITCH_TEST_MC_ERRORS_STUBS_H

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
using IOInterruptState = bool;

inline bool switch_test_interrupts_off;
inline std::vector<std::string> switch_test_lines;

inline void switch_test_may_block(const char *what)
{
    if (switch_test_interrupts_off)
        throw std::runtime_error(std::string(what) + " in an interrupt filter");
}

inline void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

inline void IOLog(const char *format, ...)
{
    switch_test_may_block("IOLog");
    char line[512];
    va_list arguments;  // NOLINT(cppcoreguidelines-init-variables): va_start initialises it
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    switch_test_lines.emplace_back(line);
}

// A panic is what a filter may do: it neither blocks nor allocates. The test catches it with the panic's text.
struct switch_test_panic : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] inline void panic(const char *format, ...) __attribute__((format(printf, 1, 2)));

inline void panic(const char *format, ...)
{
    char line[512];
    va_list arguments;  // NOLINT(cppcoreguidelines-init-variables): va_start initialises it
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    throw switch_test_panic(line);
}

struct IOSimpleLock {
    bool held;
};

inline IOSimpleLock *IOSimpleLockAlloc()
{
    switch_test_may_block("IOSimpleLockAlloc");
    return new IOSimpleLock{};
}

inline IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock)
{
    switch_test_may_block("IOSimpleLockLockDisableInterrupt");
    lock->held = true;
    return true;
}

inline void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *lock, IOInterruptState)
{
    lock->held = false;
}

class OSObject {};

class IOService : public OSObject {};

class IOInterruptEventSource : public OSObject {
public:
    using Action = void (*)(OSObject *, IOInterruptEventSource *, int);

    void enable()
    {
        enabled = true;
    }

    void disable()
    {
        enabled = false;
    }

    Action action = nullptr;
    bool enabled = false;
};

class IOFilterInterruptEventSource : public IOInterruptEventSource {
public:
    using Filter = bool (*)(OSObject *, IOFilterInterruptEventSource *);

    static IOFilterInterruptEventSource *filterInterruptEventSource(OSObject *, Action action, Filter filter,
                                                                    IOService *, int)
    {
        auto *source = new IOFilterInterruptEventSource;
        source->action = action;
        source->filter = filter;
        last() = source;
        return source;
    }

    static IOFilterInterruptEventSource *&last()
    {
        static IOFilterInterruptEventSource *source;
        return source;
    }

    // The primary interrupt: the filter with interrupts off, then the action on the work loop when it returned true.
    bool fire()
    {
        switch_test_interrupts_off = true;
        bool taken = false;
        try {
            taken = filter(nullptr, this);
        } catch (...) {
            switch_test_interrupts_off = false;
            throw;
        }
        switch_test_interrupts_off = false;
        if (taken)
            action(nullptr, this, 1);
        return taken;
    }

    Filter filter = nullptr;
};

class IOWorkLoop : public OSObject {
public:
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);

    static IOWorkLoop *workLoop()
    {
        static IOWorkLoop loop;
        return &loop;
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): stands in for IOWorkLoop's instance method
    IOReturn addEventSource(IOInterruptEventSource *)
    {
        return kIOReturnSuccess;
    }

    // The gate: the action runs as the work loop's own (no event source's action runs meanwhile).
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): a member in IOKit, called through the work loop
    IOReturn runAction(Action action, OSObject *target, void *arg0 = nullptr, void *arg1 = nullptr,
                       void *arg2 = nullptr, void *arg3 = nullptr)
    {
        switch_test_may_block("IOWorkLoop::runAction");
        return action(target, arg0, arg1, arg2, arg3);
    }
};

using wait_result_t = int;
using thread_t = void *;
using kern_return_t = int;
constexpr kern_return_t KERN_SUCCESS = 0;

// kernel_thread_start's threads, run by the test when it chooses (the GIC comes up later than the start).
inline std::vector<std::pair<void (*)(void *, wait_result_t), void *>> switch_test_threads;

inline kern_return_t kernel_thread_start(void (*body)(void *, wait_result_t), void *context, thread_t *thread)
{
    switch_test_threads.emplace_back(body, context);
    *thread = nullptr;
    return KERN_SUCCESS;
}

inline void thread_deallocate(thread_t)
{}

extern "C" uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t size);

#define NSEC_PER_USEC 1000u
constexpr uint32_t kMillisecondScale = 1000000;

// A wait: thread_block runs switch_test_while_blocked (what happens meanwhile: an interrupt, or nothing), then says
// whether a thread_wakeup on the event came.
using wait_interrupt_t = int;
using event_t = void *;
using thread_continue_t = void *;
constexpr wait_interrupt_t THREAD_UNINT = 0;
constexpr wait_result_t THREAD_WAITING = -1, THREAD_AWAKENED = 0, THREAD_TIMED_OUT = 1;
#define THREAD_CONTINUE_NULL nullptr

inline const void *switch_test_waiting_on;
inline bool switch_test_woken;
inline unsigned switch_test_blocks;
inline void (*switch_test_while_blocked)();

inline wait_result_t assert_wait_deadline(const void *event, wait_interrupt_t, uint64_t)
{
    switch_test_may_block("assert_wait_deadline");
    switch_test_waiting_on = event;
    switch_test_woken = false;
    return THREAD_WAITING;
}

inline void thread_wakeup(const void *event)
{
    switch_test_woken = switch_test_woken || event == switch_test_waiting_on;
}

inline thread_t current_thread()
{
    return nullptr;
}

inline void clear_wait(thread_t, wait_result_t)
{
    switch_test_waiting_on = nullptr;
}

inline wait_result_t thread_block(thread_continue_t)
{
    switch_test_may_block("thread_block");
    ++switch_test_blocks;
    if (switch_test_while_blocked)
        switch_test_while_blocked();
    switch_test_waiting_on = nullptr;
    return switch_test_woken ? THREAD_AWAKENED : THREAD_TIMED_OUT;
}

inline uint64_t mach_absolute_time()
{
    return 0;
}

inline void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *deadline)
{
    *deadline = 0;
}

#endif
