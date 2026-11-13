// SPDX-License-Identifier: MIT
// C++ with no ARC: objc_constructInstance, objc_destructInstance and object_dispose are unavailable under ARC, and an
// ARC -dealloc always ends in NSObject's, which frees the instance (raw_object_pool.h)
#include "raw_object_pool.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "raw_lock.h"
#include "raw_take_pool.h"
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#endif

namespace {

constexpr uint32_t kPools = 4, kKept = 32;

struct InstancePool {
    std::atomic<Class> cls{nullptr};
    size_t size = 0;
    void (*dropped)(void *) = nullptr;
    raw::TakePool<void *, kKept> blocks;
    std::atomic<uint64_t> made{0}, freed{0};
};

InstancePool gPools[kPools];
std::atomic<uint32_t> gPoolCount{0};

InstancePool *poolOf(Class cls)
{
    const uint32_t n = gPoolCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; i++)
        if (gPools[i].cls.load(std::memory_order_relaxed) == cls)
            return &gPools[i];
    return nullptr;
}

// A subclass's instance runs the pooled class's `dropped`, and is freed
InstancePool *ancestorPool(Class cls)
{
    for (Class c = cls; c; c = class_getSuperclass(c))
        if (InstancePool *pool = poolOf(c))
            return pool;
    return nullptr;
}

// Under ASan a kept block is poisoned: a pointer someone kept past the instance's dealloc faults there as it would after
// free
void hide(void *block, size_t size)
{
#if __has_feature(address_sanitizer)
    ASAN_POISON_MEMORY_REGION(block, size);
#else
    (void)block;
    (void)size;
#endif
}

void show(void *block, size_t size)
{
#if __has_feature(address_sanitizer)
    ASAN_UNPOISON_MEMORY_REGION(block, size);
#else
    (void)block;
    (void)size;
#endif
}

id pooledAlloc(id self, SEL, void *)
{
    Class cls = reinterpret_cast<Class>(self);
    InstancePool *pool = poolOf(cls);
    if (!pool)
        return class_createInstance(cls, 0);
    uint32_t node;
    void *block;
    if (void **kept = pool->blocks.take(node)) {
        block = *kept;
        pool->blocks.giveBack(node);
        show(block, pool->size);
        memset(block, 0, pool->size);
    } else {
        block = calloc(1, pool->size);
        if (!block)
            return nullptr;
        pool->made.fetch_add(1, std::memory_order_relaxed);
    }
    return objc_constructInstance(cls, block);
}

void pooledDealloc(id self, SEL)
{
    Class cls = object_getClass(self);
    InstancePool *pool = poolOf(cls);
    if (!pool) {
        if (InstancePool *ancestor = ancestorPool(cls); ancestor && ancestor->dropped)
            ancestor->dropped(self);
        object_dispose(self);
        return;
    }
    if (pool->dropped)
        pool->dropped(self);
    objc_destructInstance(self);
    uint32_t node;
    void **slot = pool->blocks.room(node);
    if (!slot) {
        pool->freed.fetch_add(1, std::memory_order_relaxed);
        free(self);
        return;
    }
    *slot = self;
    hide(self, pool->size);
    pool->blocks.keep(node);
}

}  // namespace

bool rawPoolInstances(Class cls, void (*dropped)(void *object))
{
    static raw::Mutex lock;
    std::lock_guard<raw::Mutex> l(lock);
    const uint32_t at = gPoolCount.load(std::memory_order_relaxed);
    if (!cls || poolOf(cls) || at == kPools)
        return false;
    InstancePool &pool = gPools[at];
    pool.size = class_getInstanceSize(cls);
    pool.dropped = dropped;
    pool.cls.store(cls, std::memory_order_relaxed);
    gPoolCount.store(at + 1, std::memory_order_release);
    // the dealloc first: an instance NSObject's alloc made meanwhile is a block of the class's size too
    return class_addMethod(cls, sel_registerName("dealloc"), reinterpret_cast<IMP>(pooledDealloc), "v@:") &&
           class_addMethod(object_getClass(reinterpret_cast<id>(cls)), sel_registerName("allocWithZone:"),
                           reinterpret_cast<IMP>(pooledAlloc), "@@:^{_NSZone=}");
}

uint64_t rawPoolMade(Class cls)
{
    const InstancePool *pool = poolOf(cls);
    return pool ? pool->made.load(std::memory_order_relaxed) : 0;
}

uint64_t rawPoolFreed(Class cls)
{
    const InstancePool *pool = poolOf(cls);
    return pool ? pool->freed.load(std::memory_order_relaxed) : 0;
}
