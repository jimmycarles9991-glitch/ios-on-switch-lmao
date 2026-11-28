// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

using IOReturn = int32_t;
using IOOptionBits = uint32_t;
using IOByteCount = uint64_t;
using IODirection = uint32_t;
using addr64_t = uint64_t;
using vm_size_t = size_t;
using vm_map_address_t = uintptr_t;
using mach_vm_address_t = uint64_t;
using ppnum_t = uint32_t;
using boolean_t = int;
using task_t = void *;

constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnBadArgument = -536870206;
constexpr IOReturn kIOReturnNoMemory = -536870211;
constexpr IOReturn kIOReturnUnsupported = -536870201;
constexpr IOReturn kIOReturnIOError = -536870187;

constexpr IODirection kIODirectionNone = 0;
constexpr IODirection kIODirectionIn = 1;
constexpr IODirection kIODirectionOut = 2;
constexpr IODirection kIODirectionOutIn = 3;
constexpr IOOptionBits kIOMemoryMapperNone = 0x8000;

constexpr uint64_t PAGE_SHIFT = 12;
constexpr uint64_t PAGE_SIZE = 1ull << PAGE_SHIFT;
constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;

inline uint64_t atop_64(uint64_t pa)
{
    return pa >> PAGE_SHIFT;
}

struct IOLock {
    std::mutex mutex;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
    lock->mutex.lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
    lock->mutex.unlock();
}

inline void cc_clear(size_t len, void *dst)
{
    memset(dst, 0, len);
}

struct OSSymbol {};

struct IOWorkLoop {};

struct IOMemoryMap {
    void release()
    {}

    uintptr_t getVirtualAddress() const
    {
        return 0;
    }
};

class IOMemoryDescriptor {
public:
    struct Segment {
        uint8_t *va;
        addr64_t pa;
        size_t len;
    };

    static inline unsigned withAddressRangeCalls = 0;
    static inline unsigned createMappingCalls = 0;
    static inline IODirection lastDirection = kIODirectionNone;

    static IOMemoryDescriptor *withAddressRange(mach_vm_address_t address, uint64_t length, IODirection direction,
                                                task_t);

    IOMemoryDescriptor() = default;

    explicit IOMemoryDescriptor(std::vector<Segment> segs) : segments(std::move(segs))
    {
        for (const Segment &s : segments)
            total += s.len;
    }

    uint64_t getLength() const
    {
        return total;
    }

    IOReturn prepare(IODirection = kIODirectionNone)
    {
        ++prepareCalls;
        return kIOReturnSuccess;
    }

    IOReturn complete(IODirection = kIODirectionNone)
    {
        ++completeCalls;
        return kIOReturnSuccess;
    }

    IOMemoryMap *createMappingInTask(task_t, mach_vm_address_t, IOOptionBits, uint64_t = 0, uint64_t = 0)
    {
        ++createMappingCalls;
        return nullptr;
    }

    addr64_t getPhysicalSegment(uint64_t offset, IOByteCount *length, IOOptionBits = 0) const
    {
        uint64_t cur = 0;
        for (const Segment &s : segments) {
            if (offset < cur + s.len) {
                const uint64_t off = offset - cur;
                if (length)
                    *length = s.len - off;
                return s.pa + off;
            }
            cur += s.len;
        }
        if (length)
            *length = 0;
        return 0;
    }

    IOByteCount readBytes(uint64_t offset, void *bytes, IOByteCount withLength) const
    {
        auto *dst = static_cast<uint8_t *>(bytes);
        IOByteCount done = 0;
        uint64_t cur = 0;
        for (const Segment &s : segments) {
            if (offset + done < cur + s.len) {
                const uint64_t off = (offset + done) - cur;
                const uint64_t take = (s.len - off) < (withLength - done) ? (s.len - off) : (withLength - done);
                memcpy(dst + done, s.va + off, take);
                done += take;
                if (done == withLength)
                    break;
            }
            cur += s.len;
        }
        return done;
    }

    IOByteCount writeBytes(uint64_t offset, const void *bytes, IOByteCount withLength)
    {
        const auto *src = static_cast<const uint8_t *>(bytes);
        IOByteCount done = 0;
        uint64_t cur = 0;
        for (const Segment &s : segments) {
            if (offset + done < cur + s.len) {
                const uint64_t off = (offset + done) - cur;
                const uint64_t take = (s.len - off) < (withLength - done) ? (s.len - off) : (withLength - done);
                memcpy(s.va + off, src + done, take);
                done += take;
                if (done == withLength)
                    break;
            }
            cur += s.len;
        }
        return done;
    }

    void release()
    {
        if (heapOwned)
            delete this;
    }

    std::vector<Segment> segments;
    uint64_t total = 0;
    unsigned prepareCalls = 0;
    unsigned completeCalls = 0;
    bool heapOwned = false;
};

class IOService {
public:
    virtual ~IOService() = default;

    virtual bool start(IOService *)
    {
        return true;
    }

    virtual IOWorkLoop *getWorkLoop() const
    {
        return nullptr;
    }

    virtual IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *)
    {
        return kIOReturnSuccess;
    }

    void setProperty(const char *, bool)
    {}

    void registerService()
    {}
};

enum IOAESOperation : uint32_t {
    IOAESEncrypt = 0,
    IOAESDecrypt = 1,
};

struct IOAESIV {
    unsigned char bytes[16];
};

struct IOAESKeyData {
    uint32_t bits;
    uint32_t selector;
    unsigned char bytes[32];
};

using IOAESCompletion = void (*)(void *refcon, IOReturn status);

class IOAESAccelerator : public IOService {
public:
    virtual const char *getAlgorithmName() = 0;
    virtual bool isKeySupported(unsigned selector) = 0;
    virtual unsigned getSupportedOptions() = 0;
    virtual IOReturn performAES(void *, void *, task_t, unsigned long long, IOAESIV *, IOAESOperation, IOAESKeyData *,
                                IOAESCompletion, void *) = 0;
    virtual IOReturn performAES(IOMemoryDescriptor *, IOMemoryDescriptor *, unsigned long long, IOAESIV *,
                                IOAESOperation, IOAESKeyData *, unsigned long long, unsigned long long, IOAESCompletion,
                                void *) = 0;
};

#define OSDeclareDefaultStructors(cls)
#define OSDefineMetaClassAndStructors(cls, super)

struct aes_encrypt_ctx {
    uint8_t key[32];
    int keyBytes;
};

struct aes_decrypt_ctx {
    uint8_t key[32];
    int keyBytes;
};

struct crypto_functions {
    const void *ccaes_cbc_encrypt;
    const void *ccaes_cbc_decrypt;
};

extern const crypto_functions *g_crypto_funcs;

extern "C" {
extern task_t kernel_task;
int aes_encrypt_key(const unsigned char *key, int key_len, aes_encrypt_ctx *cx);
int aes_decrypt_key(const unsigned char *key, int key_len, aes_decrypt_ctx *cx);
int aes_encrypt_cbc(const unsigned char *ibuf, const unsigned char *in_iv, unsigned int num_blk, unsigned char *obuf,
                    const aes_encrypt_ctx *cx);
int aes_decrypt_cbc(const unsigned char *ibuf, const unsigned char *in_iv, unsigned int num_blk, unsigned char *obuf,
                    const aes_decrypt_ctx *cx);
}
