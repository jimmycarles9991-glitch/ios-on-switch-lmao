// SPDX-License-Identifier: MIT
#include "apps/hwtest/bench/compiler_draw.h"
#include "apps/hwtest/bench/compiler_pixels.h"
#include "drivers/TegraGM20BMetal/cache_io.h"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace bench::compiler {
namespace {
constexpr const char *backend[] = {"paired NAK, unsplit", "NVIDIA R32.7.6"};

bool stage(gm20b::Reader &r, uint32_t type, const std::string &key, gm20b::CapturedProgram &p)
{
    uint32_t gprs = r.value<uint32_t>();
    p.image = r.list<uint8_t>();
    p.data = r.list<uint8_t>();
    if (!gprs || gprs > 255 || p.image.size() < 112 || (p.image.size() - 80) % 32 || p.data.size() > 65536)
        return false;
    uint32_t head;
    memcpy(&head, p.image.data(), 4);
    if (((head >> 10) & 15) != type)
        return false;
    p.gprs = uint8_t(gprs);
    p.id = key + (type == 1 ? "-r32-vertex" : "-r32-fragment");
    p.data.resize((p.data.size() + 255) & ~size_t(255));
    p.cbufTypes = {gm20b::CbufRoot};
    if (!p.data.empty())
        p.cbufTypes.push_back(gm20b::CbufShaderData);
    return true;
}

bool decodeEntryCompat(std::string entry, const gm20b::CacheKey &cache, gm20b::RuntimeRecord &record,
                       std::string &refusal)
{
    // Staged 24B119 .swab fixtures embed format 19 records (identical to format 20 plus a trailing 8-byte FNV-1a sum).
    if (entry.size() >= sizeof(gm20b::kEntryMagic) + 12 &&
        memcmp(entry.data(), gm20b::kEntryMagic, sizeof(gm20b::kEntryMagic)) == 0) {
        uint32_t format = 0;
        memcpy(&format, entry.data() + sizeof(gm20b::kEntryMagic), sizeof(format));
        if (format == 19 && gm20b::kEntryFormat == 20) {
            uint64_t sum = 0;
            memcpy(&sum, entry.data() + entry.size() - sizeof(sum), sizeof(sum));
            if (sum != gm20b::Pack::fnv(entry.data(), entry.size() - sizeof(sum)))
                return false;
            entry.resize(entry.size() - sizeof(sum));
            const uint32_t current = gm20b::kEntryFormat;
            memcpy(entry.data() + sizeof(gm20b::kEntryMagic), &current, sizeof(current));
        }
    }
    return gm20b::decodeEntry(entry, cache, record, refusal) == gm20b::RecordCache::Found::Record;
}

bool decode(const std::string &bytes, const char *key, Input &in)
{
    gm20b::Reader r(bytes);
    char magic[8];
    if (!r.bytes(magic, 8) || memcmp(magic, "SWAB0001", 8) || (in.key = r.text()) != key)
        return false;
    gm20b::CacheKey cache{};
    cache.key = in.key;
    std::string refusal;
    if (!decodeEntryCompat(r.text(), cache, in.nak, refusal))
        return false;
    in.samples = r.value<uint32_t>();
    in.depth = r.value<uint32_t>();
    in.colors = r.list<uint32_t>();
    in.kinds = r.list<uint8_t>();
    if ((in.samples != 1 && in.samples != 4) || (in.depth && in.depth != 260) || in.colors.empty() ||
        in.colors.size() > 3 || in.kinds.size() != gm20b::entryCount(in.nak.pipeline))
        return false;
    if (!stage(r, 1, key, in.nvidia[0]) || !stage(r, 5, key, in.nvidia[1]) || r.value<uint32_t>() != 4)
        return false;
    for (unsigned i = 0; i < 4; ++i) {
        auto &s = in.seeds.emplace_back();
        s.number = r.value<uint32_t>();
        const uint32_t count = r.value<uint32_t>();
        if (!count || count > 16)
            return false;
        for (uint32_t j = 0; j < count; ++j) {
            Block b{r.value<uint32_t>(), r.value<uint32_t>(), r.value<uint32_t>(), r.list<uint8_t>()};
            if ((b.stage != 1 && b.stage != 2) || b.slot >= 32 || b.offset > 256 || b.bytes.empty() ||
                b.bytes.size() > 65536)
                return false;
            s.blocks.emplace_back(std::move(b));
        }
    }
    return r.ok() && r.at() == bytes.size();
}

std::string pixels(const Input &in, const Result &nak, const Result &nv)
{
    if (nak.pixels.size() != nv.pixels.size())
        return "different attachment counts";
    for (size_t i = 0; i < nak.pixels.size(); ++i) {
        const Attachment plane{i, i == in.colors.size(), width, height, in.samples};
        auto mismatch = difference(plane, nak.pixels[i], nv.pixels[i], backend[0], backend[1]);
        if (!mismatch.empty())
            return mismatch;
    }
    return {};
}

void timing(std::ostringstream &s, Result &r)
{
    std::sort(r.times.begin(), r.times.end());
    const auto a = r.times[49], b = r.times[50];
    s << r.times.front() << "/" << a + (b - a) / 2;
}

void describe(std::ostringstream &s, const gm20b::CapturedProgram &p)
{
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(p.image.data(), static_cast<CC_LONG>(p.image.size()), digest);
    s << p.id << " (image sha256 " << std::hex << std::setfill('0');
    for (uint8_t byte : digest)
        s << std::setw(2) << unsigned(byte);
    s << std::dec << ")";
}

bool compare(const Input &in, std::ostringstream &s)
{
    for (unsigned arm = 0; arm < 2; ++arm) {
        s << backend[arm] << " VS ";
        describe(s, in.program(arm, 0));
        s << " FS ";
        describe(s, in.program(arm, 1));
        s << "\n";
    }
    bool ok = true;
    for (size_t i = 0; i < in.seeds.size(); ++i) {
        Result arms[2];
        s << "seed " << in.seeds[i].number << " ";
        for (unsigned k = 0; k < 2; ++k) {
            const unsigned arm = (i + k) % 2;
            std::string why;
            if (!run(in, in.seeds[i], arm, i, arms[arm], why)) {
                s << backend[arm] << " failed: " << why;
                return false;
            }
        }
        auto difference = pixels(in, arms[0], arms[1]);
        const uint64_t full = uint64_t(width) * height * in.samples;
        const bool covered = arms[0].visible == full && arms[1].visible == full;
        const bool variation = i != 0 || !in.compositor() || (arms[0].varied && arms[1].varied);
        const bool wrote = arms[0].changed && arms[1].changed;
        // An edge seed can legitimately preserve every texel (GlowAlpha takes min with destination alpha).
        const bool exercised = covered && variation && (i != 0 || wrote);
        ok &= exercised && difference.empty();
        s << (difference.empty() ? (exercised ? "exact match" : "equal bytes, inconclusive coverage/write/variation")
                                 : difference)
          << "\n";
        for (unsigned arm = 0; arm < 2; ++arm) {
            s << "  " << backend[arm] << " visible " << arms[arm].visible << " changed color " << arms[arm].changed
              << " GPU ns min/median (100 draws) ";
            timing(s, arms[arm]);
            s << " GPC PLL kHz before/after " << arms[arm].clockBefore << "/" << arms[arm].clockAfter << "\n";
        }
    }
    return ok;
}
}  // namespace
}  // namespace bench::compiler

extern "C" int switch_compiler_compare(const char *key, const uint8_t *data, size_t n, char *message, size_t room)
{
    using namespace bench::compiler;
    Input input;
    std::ostringstream report;
    bool ok =
        key && data && n <= 2 * 1024 * 1024 && decode(std::string(reinterpret_cast<const char *>(data), n), key, input);
    if (ok)
        ok = compare(input, report);
    else
        report << "invalid 24B119 compiler comparison input";
    snprintf(message, room, "%s", report.str().c_str());
    return ok;
}
