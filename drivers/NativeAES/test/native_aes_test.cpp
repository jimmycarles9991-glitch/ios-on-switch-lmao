// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "../NativeAES.cpp"

namespace aes_test {
int kKernelTaskTag;
int kUserTaskTag;
const int kDummyMode = 1;
const crypto_functions kCryptoFuncs = {&kDummyMode, &kDummyMode};

struct DramRegion {
    uint8_t *va;
    addr64_t pa;
    size_t len;
    unsigned cacheAttr;
};

std::vector<DramRegion> gRegions;
unsigned gDigestCalls = 0;
unsigned gEncryptKeyCalls = 0;
unsigned gDecryptKeyCalls = 0;

void resetCounters()
{
    IOMemoryDescriptor::withAddressRangeCalls = 0;
    IOMemoryDescriptor::createMappingCalls = 0;
    IOMemoryDescriptor::lastDirection = kIODirectionNone;
    gDigestCalls = 0;
    gEncryptKeyCalls = 0;
    gDecryptKeyCalls = 0;
}

uint8_t rotl(uint8_t v, unsigned s)
{
    return static_cast<uint8_t>((v << s) | (v >> (8 - s)));
}

uint8_t rotr(uint8_t v, unsigned s)
{
    return static_cast<uint8_t>((v >> s) | (v << (8 - s)));
}

void encryptBlock(const uint8_t in[16], uint8_t out[16], const aes_encrypt_ctx *cx)
{
    for (unsigned i = 0; i < 16; ++i) {
        const uint8_t k = cx->key[i % static_cast<unsigned>(cx->keyBytes)];
        out[i] = rotl(static_cast<uint8_t>(in[i] ^ k ^ (0x31u + i)), (i & 3u) + 1u);
    }
}

void decryptBlock(const uint8_t in[16], uint8_t out[16], const aes_decrypt_ctx *cx)
{
    for (unsigned i = 0; i < 16; ++i) {
        const uint8_t k = cx->key[i % static_cast<unsigned>(cx->keyBytes)];
        out[i] = static_cast<uint8_t>(rotr(in[i], (i & 3u) + 1u) ^ k ^ (0x31u + i));
    }
}
}  // namespace aes_test

const crypto_functions *g_crypto_funcs = &aes_test::kCryptoFuncs;

extern "C" {
task_t kernel_task = &aes_test::kKernelTaskTag;
unsigned long gPhysBase = 0x80000000UL;
unsigned long gPhysSize = 0x10000000UL;

boolean_t pmap_valid_page(ppnum_t pn)
{
    const uint64_t pa = static_cast<uint64_t>(pn) << PAGE_SHIFT;
    for (const aes_test::DramRegion &r : aes_test::gRegions) {
        if (pa >= r.pa && pa < r.pa + r.len)
            return 1;
    }
    return 0;
}

unsigned int pmap_cache_attributes(ppnum_t pn)
{
    const uint64_t pa = static_cast<uint64_t>(pn) << PAGE_SHIFT;
    for (const aes_test::DramRegion &r : aes_test::gRegions) {
        if (pa >= r.pa && pa < r.pa + r.len)
            return r.cacheAttr;
    }
    return 0;
}

vm_map_address_t phystokv_range(addr64_t pa, vm_size_t *max_len)
{
    for (const aes_test::DramRegion &r : aes_test::gRegions) {
        if (pa >= r.pa && pa < r.pa + r.len) {
            const size_t pageRoom = PAGE_SIZE - (pa & PAGE_MASK);
            const size_t regionRoom = static_cast<size_t>(r.pa + r.len - pa);
            const size_t cap = pageRoom < regionRoom ? pageRoom : regionRoom;
            if (*max_len > cap)
                *max_len = cap;
            return reinterpret_cast<vm_map_address_t>(r.va + (pa - r.pa));
        }
    }
    return 0;
}

int tegra_se_aes(tegra_se_mode, bool, const uint8_t *, unsigned, uint8_t *, const void *, void *, size_t)
{
    return -1;
}

void native_aks_device_key_start(void)
{}

bool native_aks_device_digest(const char *label, uint8_t out[32])
{
    ++aes_test::gDigestCalls;
    uint32_t h = 2166136261u;
    for (const char *p = label; *p; ++p)
        h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
    for (unsigned i = 0; i < 32; ++i) {
        h = h * 1664525u + 1013904223u;
        out[i] = static_cast<uint8_t>(h >> 24);
    }
    return true;
}

void switch_refusal_k(uint64_t, const char *, const char *, const char *, ...)
{}

int aes_encrypt_key(const unsigned char *key, int key_len, aes_encrypt_ctx *cx)
{
    ++aes_test::gEncryptKeyCalls;
    cx->keyBytes = key_len;
    memcpy(cx->key, key, static_cast<size_t>(key_len));
    return 0;
}

int aes_decrypt_key(const unsigned char *key, int key_len, aes_decrypt_ctx *cx)
{
    ++aes_test::gDecryptKeyCalls;
    cx->keyBytes = key_len;
    memcpy(cx->key, key, static_cast<size_t>(key_len));
    return 0;
}

int aes_encrypt_cbc(const unsigned char *ibuf, const unsigned char *in_iv, unsigned int num_blk, unsigned char *obuf,
                    const aes_encrypt_ctx *cx)
{
    uint8_t chain[16];
    memcpy(chain, in_iv, 16);
    for (unsigned int b = 0; b < num_blk; ++b) {
        uint8_t x[16];
        for (unsigned i = 0; i < 16; ++i)
            x[i] = ibuf[b * 16 + i] ^ chain[i];
        aes_test::encryptBlock(x, obuf + b * 16, cx);
        memcpy(chain, obuf + b * 16, 16);
    }
    return 0;
}

int aes_decrypt_cbc(const unsigned char *ibuf, const unsigned char *in_iv, unsigned int num_blk, unsigned char *obuf,
                    const aes_decrypt_ctx *cx)
{
    uint8_t chain[16];
    memcpy(chain, in_iv, 16);
    for (unsigned int b = 0; b < num_blk; ++b) {
        uint8_t cipher[16];
        memcpy(cipher, ibuf + b * 16, 16);
        uint8_t dec[16];
        aes_test::decryptBlock(cipher, dec, cx);
        for (unsigned i = 0; i < 16; ++i)
            obuf[b * 16 + i] = dec[i] ^ chain[i];
        memcpy(chain, cipher, 16);
    }
    return 0;
}
}

IOMemoryDescriptor *IOMemoryDescriptor::withAddressRange(mach_vm_address_t address, uint64_t length,
                                                         IODirection direction, task_t)
{
    ++withAddressRangeCalls;
    lastDirection = direction;
    auto *va = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(address));
    addr64_t pa = 0x90000000ULL;
    for (const aes_test::DramRegion &r : aes_test::gRegions) {
        if (va >= r.va && va + length <= r.va + r.len) {
            pa = r.pa + static_cast<uint64_t>(va - r.va);
            break;
        }
    }
    auto *desc = new IOMemoryDescriptor({{va, pa, static_cast<size_t>(length)}});
    desc->heapOwned = true;
    return desc;
}

namespace {
IOAESKeyData makeSoftKey(uint32_t bits)
{
    IOAESKeyData key{};
    key.bits = bits;
    key.selector = 0;
    for (unsigned i = 0; i < 32; ++i)
        key.bytes[i] = static_cast<uint8_t>(0xa0 + i);
    return key;
}

TEST(NativeAESTest, KernelTaskOperatesDirectlyWithZeroDescriptorsOrMappings)
{
    aes_test::resetCounters();
    NativeAES aes;
    std::vector<uint8_t> plain(128);
    for (size_t i = 0; i < plain.size(); ++i)
        plain[i] = static_cast<uint8_t>(i * 7 + 3);
    std::vector<uint8_t> buf = plain;
    IOAESKeyData key = makeSoftKey(128);
    IOAESIV iv0{};
    for (unsigned i = 0; i < 16; ++i)
        iv0.bytes[i] = static_cast<uint8_t>(i + 1);

    IOAESIV encIv = iv0;
    ASSERT_EQ(
        aes.performAES(buf.data(), buf.data(), kernel_task, buf.size(), &encIv, IOAESEncrypt, &key, nullptr, nullptr),
        kIOReturnSuccess);
    EXPECT_EQ(IOMemoryDescriptor::withAddressRangeCalls, 0u);
    EXPECT_EQ(IOMemoryDescriptor::createMappingCalls, 0u);
    EXPECT_EQ(memcmp(encIv.bytes, buf.data() + buf.size() - 16, 16), 0);

    uint8_t lastCipher[16];
    memcpy(lastCipher, buf.data() + buf.size() - 16, 16);
    IOAESIV decIv = iv0;
    ASSERT_EQ(
        aes.performAES(buf.data(), buf.data(), kernel_task, buf.size(), &decIv, IOAESDecrypt, &key, nullptr, nullptr),
        kIOReturnSuccess);
    EXPECT_EQ(IOMemoryDescriptor::withAddressRangeCalls, 0u);
    EXPECT_EQ(IOMemoryDescriptor::createMappingCalls, 0u);
    EXPECT_EQ(buf, plain);
    EXPECT_EQ(memcmp(decIv.bytes, lastCipher, 16), 0);
}

TEST(NativeAESTest, DescriptorUsesPhysicalApertureWithZeroKernelMappingsAndPreservesInPlaceIv)
{
    aes_test::resetCounters();
    aes_test::gRegions.clear();
    alignas(4096) uint8_t pageA[PAGE_SIZE];
    alignas(4096) uint8_t pageB[PAGE_SIZE];
    aes_test::gRegions.push_back({pageA, 0x80100000ULL, PAGE_SIZE, 0x2});
    aes_test::gRegions.push_back({pageB, 0x80200000ULL, PAGE_SIZE, 0x2});

    std::vector<uint8_t> plain(2 * PAGE_SIZE);
    for (size_t i = 0; i < plain.size(); ++i)
        plain[i] = static_cast<uint8_t>((i * 13 + 5) & 0xff);
    memcpy(pageA, plain.data(), PAGE_SIZE);
    memcpy(pageB, plain.data() + PAGE_SIZE, PAGE_SIZE);

    IOMemoryDescriptor desc1({{pageA, 0x80100000ULL, PAGE_SIZE}, {pageB, 0x80200000ULL, PAGE_SIZE}});
    IOMemoryDescriptor desc2({{pageA, 0x80100000ULL, PAGE_SIZE}, {pageB, 0x80200000ULL, PAGE_SIZE}});

    NativeAES aes;
    IOAESKeyData key = makeSoftKey(256);
    IOAESIV iv0{};
    for (unsigned i = 0; i < 16; ++i)
        iv0.bytes[i] = static_cast<uint8_t>(0x40 + i);

    IOAESIV encIv = iv0;
    ASSERT_EQ(aes.performAES(&desc1, &desc1, 2 * PAGE_SIZE, &encIv, IOAESEncrypt, &key, 0, 0, nullptr, nullptr),
              kIOReturnSuccess);
    EXPECT_EQ(IOMemoryDescriptor::createMappingCalls, 0u);
    EXPECT_EQ(aes_test::gEncryptKeyCalls, 1u) << "key must expand once across all segments";
    EXPECT_EQ(desc1.prepareCalls, 1u);
    EXPECT_EQ(desc1.completeCalls, 1u);

    uint8_t lastCipher[16];
    memcpy(lastCipher, pageB + PAGE_SIZE - 16, 16);

    // Decrypt using two distinct descriptor objects aliasing the exact same physical pages.
    IOAESIV decIv = iv0;
    ASSERT_EQ(aes.performAES(&desc1, &desc2, 2 * PAGE_SIZE, &decIv, IOAESDecrypt, &key, 0, 0, nullptr, nullptr),
              kIOReturnSuccess);
    EXPECT_EQ(IOMemoryDescriptor::createMappingCalls, 0u);
    EXPECT_EQ(aes_test::gDecryptKeyCalls, 1u);
    EXPECT_EQ(memcmp(decIv.bytes, lastCipher, 16), 0);
    EXPECT_EQ(memcmp(pageA, plain.data(), PAGE_SIZE), 0);
    EXPECT_EQ(memcmp(pageB, plain.data() + PAGE_SIZE, PAGE_SIZE), 0);
}

TEST(NativeAESTest, CrossPageStraddlingBlockAndUncachedPageFallback)
{
    aes_test::resetCounters();
    aes_test::gRegions.clear();
    alignas(4096) uint8_t pageA[PAGE_SIZE];
    alignas(4096) uint8_t pageB[PAGE_SIZE];
    // pageA is cacheable DRAM, pageB is uncached (WIMG = 0x4): must fall back to readBytes/writeBytes safely.
    aes_test::gRegions.push_back({pageA, 0x80300000ULL, PAGE_SIZE, 0x2});
    aes_test::gRegions.push_back({pageB, 0x80400000ULL, PAGE_SIZE, 0x4});

    memset(pageA, 0, PAGE_SIZE);
    memset(pageB, 0, PAGE_SIZE);
    const uint64_t offset = PAGE_SIZE - 8;
    const size_t len = 64;
    std::vector<uint8_t> plain(len);
    for (size_t i = 0; i < len; ++i)
        plain[i] = static_cast<uint8_t>(0x20 + i);
    IOMemoryDescriptor desc({{pageA, 0x80300000ULL, PAGE_SIZE}, {pageB, 0x80400000ULL, PAGE_SIZE}});
    ASSERT_EQ(desc.writeBytes(offset, plain.data(), len), len);

    NativeAES aes;
    IOAESKeyData key = makeSoftKey(128);
    IOAESIV iv0{};
    for (unsigned i = 0; i < 16; ++i)
        iv0.bytes[i] = static_cast<uint8_t>(0x80 + i);

    IOAESIV encIv = iv0;
    ASSERT_EQ(aes.performAES(&desc, &desc, len, &encIv, IOAESEncrypt, &key, offset, offset, nullptr, nullptr),
              kIOReturnSuccess);
    EXPECT_EQ(IOMemoryDescriptor::createMappingCalls, 0u);

    uint8_t lastCipher[16];
    ASSERT_EQ(desc.readBytes(offset + len - 16, lastCipher, 16), 16u);
    IOAESIV decIv = iv0;
    ASSERT_EQ(aes.performAES(&desc, &desc, len, &decIv, IOAESDecrypt, &key, offset, offset, nullptr, nullptr),
              kIOReturnSuccess);
    EXPECT_EQ(IOMemoryDescriptor::createMappingCalls, 0u);
    EXPECT_EQ(memcmp(decIv.bytes, lastCipher, 16), 0);

    std::vector<uint8_t> out(len);
    ASSERT_EQ(desc.readBytes(offset, out.data(), len), len);
    EXPECT_EQ(out, plain);
}

TEST(NativeAESTest, UserTaskInPlaceCreatesSingleDescriptor)
{
    aes_test::resetCounters();
    aes_test::gRegions.clear();
    alignas(4096) uint8_t buf[PAGE_SIZE];
    aes_test::gRegions.push_back({buf, 0x80500000ULL, PAGE_SIZE, 0x2});
    for (size_t i = 0; i < 64; ++i)
        buf[i] = static_cast<uint8_t>(i ^ 0x5c);
    std::vector<uint8_t> plain(buf, buf + 64);

    NativeAES aes;
    IOAESKeyData key = makeSoftKey(128);
    IOAESIV iv0{};
    IOAESIV encIv = iv0;
    ASSERT_EQ(aes.performAES(buf, buf, &aes_test::kUserTaskTag, 64, &encIv, IOAESEncrypt, &key, nullptr, nullptr),
              kIOReturnSuccess);
    EXPECT_EQ(IOMemoryDescriptor::withAddressRangeCalls, 1u);
    EXPECT_EQ(IOMemoryDescriptor::lastDirection, kIODirectionOutIn);
    EXPECT_EQ(IOMemoryDescriptor::createMappingCalls, 0u);

    IOAESIV decIv = iv0;
    ASSERT_EQ(aes.performAES(buf, buf, &aes_test::kUserTaskTag, 64, &decIv, IOAESDecrypt, &key, nullptr, nullptr),
              kIOReturnSuccess);
    EXPECT_EQ(IOMemoryDescriptor::withAddressRangeCalls, 2u);
    EXPECT_EQ(IOMemoryDescriptor::createMappingCalls, 0u);
    EXPECT_EQ(memcmp(buf, plain.data(), 64), 0);
}

TEST(NativeAESTest, DeviceKeysAreCachedAndSchedulesReusedAcrossCalls)
{
    aes_test::resetCounters();
    NativeAES aes;
    uint8_t buf[32] = {};
    for (unsigned i = 0; i < 32; ++i)
        buf[i] = static_cast<uint8_t>(i + 1);
    std::vector<uint8_t> plain(buf, buf + 32);

    IOAESKeyData key2000{};
    key2000.bits = 256;
    key2000.selector = 2000;
    IOAESKeyData key2212{};
    key2212.bits = 128;
    key2212.selector = 2212;
    IOAESKeyData invalidKey{};
    invalidKey.bits = 128;
    invalidKey.selector = 500;

    IOAESIV iv{};
    EXPECT_EQ(aes.performAES(buf, buf, kernel_task, 32, &iv, IOAESEncrypt, &invalidKey, nullptr, nullptr),
              kIOReturnUnsupported);
    EXPECT_EQ(
        aes.performAES(buf, buf, kernel_task, 32, &iv, static_cast<IOAESOperation>(99), &key2000, nullptr, nullptr),
        kIOReturnUnsupported);
    EXPECT_EQ(aes_test::gDigestCalls, 0u);

    for (unsigned round = 0; round < 5; ++round) {
        IOAESIV encIv{};
        IOAESIV decIv{};
        ASSERT_EQ(aes.performAES(buf, buf, kernel_task, 32, &encIv, IOAESEncrypt, &key2000, nullptr, nullptr),
                  kIOReturnSuccess);
        ASSERT_EQ(aes.performAES(buf, buf, kernel_task, 32, &decIv, IOAESDecrypt, &key2000, nullptr, nullptr),
                  kIOReturnSuccess);
        EXPECT_EQ(memcmp(buf, plain.data(), 32), 0);
    }
    EXPECT_EQ(aes_test::gDigestCalls, 1u);
    EXPECT_EQ(aes_test::gEncryptKeyCalls, 1u);
    EXPECT_EQ(aes_test::gDecryptKeyCalls, 1u);

    for (unsigned round = 0; round < 3; ++round) {
        IOAESIV encIv{};
        ASSERT_EQ(aes.performAES(buf, buf, kernel_task, 32, &encIv, IOAESEncrypt, &key2212, nullptr, nullptr),
                  kIOReturnSuccess);
    }
    EXPECT_EQ(aes_test::gDigestCalls, 2u);
    EXPECT_EQ(aes_test::gEncryptKeyCalls, 2u);
    EXPECT_EQ(aes_test::gDecryptKeyCalls, 2u);
}
}  // namespace
