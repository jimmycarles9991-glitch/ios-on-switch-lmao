// The file SwitchNVRAM keeps the NVRAM in (drivers/SwitchNVRAM/SwitchNVRAMStore.h): its layout, round trips, the
// newest-valid-slot rule, and what a damaged file falls back to. Banks are built here as IODTNVRAM serializes them
// (CHRP header checksum, adler32 from the generation on) and checked against zlib, an implementation of its own.
#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "bazel/testing/require.h"
#include "switch_nvram_store_harness.h"

namespace {

using Bytes = std::vector<uint8_t>;
constexpr uint32_t kBank = 8192;
constexpr uint32_t kHeader = 32;

Bytes str(const std::string &s)
{
    return Bytes(s.begin(), s.end());
}

Bytes operator+(Bytes a, const Bytes &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

void put32(uint8_t *at, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        at[i] = (uint8_t)(v >> (8 * i));
}

void append(Bytes &out, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        out.push_back((uint8_t)(v >> (8 * i)));
}

uint32_t zlibCrc32(const Bytes &data)
{
    return (uint32_t)crc32(0, data.data(), (uInt)data.size());
}

uint32_t zlibAdler32(const uint8_t *data, size_t length)
{
    return (uint32_t)adler32(1, data, (uInt)length);
}

// A CHRP partition header ('<BBH12s'): signature 0x70, checksum, length in 16-byte blocks, name.
Bytes chrpHeader(const std::string &name, uint32_t size)
{
    Bytes raw(16, 0);
    raw[0] = 0x70;
    raw[2] = (uint8_t)(size / 16);
    raw[3] = (uint8_t)(size / 16 >> 8);
    std::memcpy(raw.data() + 4, name.data(), std::min<size_t>(name.size(), 12));
    uint32_t checksum = raw[0];
    for (int i = 2; i < 16; ++i)
        checksum += raw[i];
    while (checksum > 255)
        checksum = (checksum & 255) + (checksum >> 8);
    raw[1] = (uint8_t)checksum;
    return raw;
}

using Variables = std::vector<std::pair<std::string, std::string>>;

// A bank as IONVRAMCHRPHandler::serializeVariables writes it, with the packaging's proxy layout (common 2 KiB).
Bytes bank(const Variables &variables = {}, uint32_t generation = 1, uint32_t size = kBank,
           const std::string &name = "nvram")
{
    Bytes image(size, 0);
    const auto place = [&](size_t at, const Bytes &bytes) {
        std::memcpy(image.data() + at, bytes.data(), bytes.size());
    };
    place(0, chrpHeader(name, 32));
    place(32, chrpHeader("common", 16 + 2048));
    place(48 + 2048, chrpHeader("free", size - 48 - 2048));
    Bytes body;
    for (const auto &[key, value] : variables)
        body = body + str(key) + Bytes{'='} + str(value) + Bytes{0};
    place(48, body);
    put32(image.data() + 20, generation);
    put32(image.data() + 16, zlibAdler32(image.data() + 20, image.size() - 20));
    return image;
}

uint32_t stride(uint32_t size = kBank)
{
    return (kHeader + size + 4095) / 4096 * 4096;
}

struct SlotOptions {
    uint32_t version = 1;
    std::string magic = "SWCHNVRM";
    uint32_t reserved = 0;
};

Bytes slot(const Bytes &image, uint64_t sequence, const SlotOptions &o = {})
{
    Bytes header = str(o.magic);
    append(header, o.version, 4);
    append(header, image.size(), 4);
    append(header, sequence, 8);
    append(header, o.reserved, 4);
    Bytes data = header;
    append(data, zlibCrc32(header + image), 4);
    data = data + image;
    data.resize(stride((uint32_t)image.size()), 0);
    return data;
}

Bytes randomBytes(size_t n, uint32_t seed)
{
    std::mt19937 random(seed);
    Bytes out(n);
    for (uint8_t &b : out)
        b = (uint8_t)random();
    return out;
}

Bytes fill(const Bytes &image, uint64_t sequence)
{
    Bytes buffer(nv_stride((uint32_t)image.size()), 0);
    nv_fill(buffer.data(), image.data(), (uint32_t)image.size(), sequence);
    return buffer;
}

using Pick = std::pair<int, uint64_t>;

Pick pick(const Bytes &data, uint32_t size = kBank)
{
    uint64_t sequence = 0;
    const int index = nv_pick(data.data(), data.size(), size, &sequence);
    return {index, sequence};
}

Bytes imageOf(const Bytes &data, int index, uint32_t size = kBank)
{
    const size_t start = (size_t)index * stride(size) + kHeader;
    REQUIRE(start + size <= data.size());
    return Bytes(data.begin() + (long)start, data.begin() + (long)(start + size));
}

// The driver's write: the slot that does not hold the current bank (SwitchNVRAM::writeOut).
std::pair<Bytes, int> write(Bytes data, int current, const Bytes &image, uint64_t sequence)
{
    const int target = current ^ 1;
    if (data.size() < 2 * stride())
        data.resize(2 * stride(), 0);
    const Bytes filled = fill(image, sequence);
    std::memcpy(data.data() + (size_t)target * stride(), filled.data(), stride());
    return {data, target};
}

TEST(SwitchNVRAMStore, Crc32IsZlibs)
{
    for (const Bytes &data : {Bytes{}, str("123456789"), randomBytes(1, 1), randomBytes(8192 + 33, 2)})
        EXPECT_EQ(nv_crc32(data.data(), data.size()), zlibCrc32(data)) << data.size() << " bytes";
}

TEST(SwitchNVRAMStore, StrideIsHeaderAndBankIn4kBlocks)
{
    EXPECT_EQ(nv_stride(8192), 12288u);
    EXPECT_EQ(nv_stride(4096 - 32), 4096u);
    EXPECT_EQ(nv_stride(65536), 69632u);
}

TEST(SwitchNVRAMStore, SlotLayout)
{
    const Bytes image = bank({{"boot-args", "-v"}}, 7);
    const Bytes expected = slot(image, 0x100000002);
    EXPECT_EQ(fill(image, 0x100000002), expected);
    Bytes inPlace(nv_stride((uint32_t)image.size()), 0xa5);
    std::memcpy(inPlace.data() + kHeader, image.data(), image.size());
    nv_fill(inPlace.data(), inPlace.data() + kHeader, (uint32_t)image.size(), 0x100000002);
    EXPECT_EQ(inPlace, expected);
}

TEST(SwitchNVRAMStore, ChrpBanksIodtnvramAccepts)
{
    EXPECT_TRUE(nv_chrp_valid(bank().data(), kBank));
    EXPECT_TRUE(nv_chrp_valid(bank({{"fm-spstatus", "abc"}, {"ldm", "true"}}).data(), kBank));
    EXPECT_TRUE(nv_chrp_valid(bank({}, 1, kBank, "2nvram").data(), kBank));
}

TEST(SwitchNVRAMStore, ChrpBanksIodtnvramRefuses)
{
    const Bytes good = bank({{"StartupMute", "0"}});
    Bytes badAdler = good;
    badAdler[50] ^= 1;
    Bytes badChecksum = good;
    badChecksum[1] ^= 1;
    EXPECT_FALSE(nv_chrp_valid(badAdler.data(), kBank));
    EXPECT_FALSE(nv_chrp_valid(badChecksum.data(), kBank));
    EXPECT_FALSE(nv_chrp_valid(bank({}, 1, kBank, "common").data(), kBank));
    EXPECT_FALSE(nv_chrp_valid(Bytes(kBank, 0).data(), kBank));
    const Bytes head(good.begin(), good.begin() + 16);
    EXPECT_FALSE(nv_chrp_valid(head.data(), 16));
}

TEST(SwitchNVRAMStore, RoundTrip)
{
    const Bytes image = bank({{"boot-args", ""}, {"fm-spstatus", "ab"}});
    const auto [data, current] = write({}, 1, image, 1);
    EXPECT_EQ(current, 0);
    EXPECT_EQ(pick(data), Pick(0, 1));
    EXPECT_EQ(imageOf(data, 0), image);
}

TEST(SwitchNVRAMStore, WritesAlternateAndTheNewestWins)
{
    Bytes data;
    int current = 1;
    for (uint64_t sequence = 1; sequence < 8; ++sequence) {
        const Bytes image = bank({{"count", std::to_string(sequence)}}, (uint32_t)sequence);
        std::tie(data, current) = write(data, current, image, sequence);
        EXPECT_EQ(pick(data), Pick(current, sequence));
        EXPECT_EQ(imageOf(data, current), image);
    }
    EXPECT_EQ(data.size(), 2 * stride());
}

TEST(SwitchNVRAMStore, SequenceIs64Bit)
{
    const Bytes older = bank({{"a", "1"}}), newer = bank({{"a", "2"}});
    EXPECT_EQ(pick(fill(older, 0xffffffff) + fill(newer, 0x100000000)), Pick(1, 0x100000000));
    EXPECT_EQ(pick(fill(newer, 0x100000000) + fill(older, 0xffffffff)), Pick(0, 0x100000000));
}

TEST(SwitchNVRAMStore, TornNewestWriteFallsBackToThePreviousBank)
{
    std::vector<Bytes> banks;
    for (uint32_t n : {1u, 2u, 3u})
        banks.push_back(bank({{"v", std::string(40, (char)('0' + n))}}, n));
    auto [data, current] = write({}, 1, banks[0], 1);
    std::tie(data, current) = write(data, current, banks[1], 2);
    const auto [complete, target] = write(data, current, banks[2], 3);
    EXPECT_EQ(pick(complete), Pick(target, 3));
    const size_t start = (size_t)target * stride();
    // A power cut during write 3: its slot holds the new bytes up to the cut and write 1's after it.
    for (size_t cut : {(size_t)8, (size_t)kHeader, (size_t)kHeader + 48 + 3, (size_t)kHeader + 48 + 39}) {
        Bytes torn = data;
        std::memcpy(torn.data() + start, complete.data() + start, cut);
        EXPECT_EQ(pick(torn), Pick(current, 2)) << "cut at " << cut;
        EXPECT_EQ(imageOf(torn, current), banks[1]) << "cut at " << cut;
    }
}

TEST(SwitchNVRAMStore, AFlippedByteAnywhereCrcCoversRejectsThatSlot)
{
    const Bytes good = fill(bank({{"boot-args", "-v"}}), 5);
    std::vector<size_t> positions;
    for (size_t at = 0; at < kHeader; ++at)
        positions.push_back(at);
    positions.insert(positions.end(), {kHeader, kHeader + 48, kHeader + kBank - 1});
    for (size_t at : positions) {
        Bytes damaged = good;
        damaged[at] ^= 0x40;
        EXPECT_EQ(pick(damaged), Pick(-1, 0)) << "byte " << at;
    }
    Bytes damaged = good;
    damaged[kHeader + kBank] ^= 0x40;  // padding: not part of the bank, ignored
    EXPECT_EQ(pick(damaged), Pick(0, 5));
}

TEST(SwitchNVRAMStore, ValidCrcOverABankIodtnvramWouldPanicOn)
{
    Bytes image = bank({{"boot-args", "-v"}});
    image[60] ^= 1;  // adler now wrong: IONVRAMCHRPHandler::unserializeImage panics on this
    EXPECT_EQ(pick(slot(image, 3)), Pick(-1, 0));
}

TEST(SwitchNVRAMStore, OtherVersionsMagicsSizesAndReservedBits)
{
    const Bytes image = bank();
    EXPECT_EQ(pick(slot(image, 1, {.version = 2})), Pick(-1, 0));
    EXPECT_EQ(pick(slot(image, 1, {.magic = "SWCHNVRX"})), Pick(-1, 0));
    EXPECT_EQ(pick(slot(image, 1, {.reserved = 1})), Pick(-1, 0));
    const Bytes small = bank({}, 1, 4096);
    EXPECT_EQ(pick(slot(small, 1), kBank), Pick(-1, 0));
    EXPECT_EQ(pick(slot(small, 1), 4096), Pick(0, 1));
}

TEST(SwitchNVRAMStore, ShortEmptyAndGarbageFilesStartEmpty)
{
    const Bytes good = fill(bank(), 9);
    EXPECT_EQ(pick({}), Pick(-1, 0));
    EXPECT_EQ(pick(Bytes(good.begin(), good.begin() + kHeader + kBank - 1)), Pick(-1, 0));
    EXPECT_EQ(pick(Bytes(good.begin(), good.begin() + kHeader + kBank)), Pick(0, 9));
    EXPECT_EQ(pick(randomBytes(2 * stride(), 3)), Pick(-1, 0));
    EXPECT_EQ(pick(Bytes(2 * stride(), 0)), Pick(-1, 0));
    // Slot 1 cut short by a power cut while the file grew: slot 0 still counts.
    const Bytes next = fill(bank(), 10);
    EXPECT_EQ(pick(good + Bytes(next.begin(), next.begin() + 200)), Pick(0, 9));
}

// Every length of a two-slot file, each in an allocation of exactly that size: ASan (the harness and the header are
// built with it) catches a read past the end.
TEST(SwitchNVRAMStore, EveryFileLengthUnderAddressSanitizer)
{
    const Bytes image = bank({{"boot-args", "-v"}});
    REQUIRE(nv_chrp_valid(image.data(), kBank));
    const uint32_t slotBytes = nv_stride(kBank);
    const Bytes file = fill(image, 1) + fill(image, 2);
    REQUIRE(file.size() == 2u * slotBytes);
    unsigned cases = 0;
    for (size_t length = 0; length <= 2u * slotBytes; ++length) {
        auto *copy = static_cast<uint8_t *>(std::malloc(length ? length : 1));
        REQUIRE(copy != nullptr);
        std::memcpy(copy, file.data(), length);
        uint64_t sequence = 0;
        const int picked = nv_pick(copy, length, kBank, &sequence);
        std::free(copy);
        const int expected = length >= slotBytes + kHeader + kBank ? 1 : length >= kHeader + kBank ? 0 : -1;
        ASSERT_EQ(picked, expected) << "length " << length;
        if (picked >= 0)
            ASSERT_EQ(sequence, (uint64_t)picked + 1) << "length " << length;
        ++cases;
    }
    EXPECT_EQ(cases, 24577u);
}

}  // namespace
