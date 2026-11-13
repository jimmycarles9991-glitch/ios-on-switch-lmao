// SPDX-License-Identifier: MIT
// See raw_cache.h.
#include "raw_cache.h"
#include "raw_cache_io.h"
#include "../../../../kits/SwitchSeedPack/SwitchSeedPack.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace raw {
namespace {

constexpr uint8_t kSplitTried = 1, kSplit = 2;
constexpr uint64_t kSizeLimit = 1u << 24;  // an entry larger than any record could be: a broken file
constexpr time_t kStaleTemporarySeconds = 60;

std::string hex16(uint64_t v)
{
    char b[17];
    snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

void writeProgram(Writer &w, const CapturedProgram &p)
{
    w.text(p.id);
    w.list(p.image);
    w.list(p.data);
    w.list(p.cbufTypes);
    w.value(p.gprs);
    w.value(p.boundTextures);
}

CapturedProgram readProgram(Reader &r)
{
    CapturedProgram p;
    p.id = r.text();
    p.image = r.list<uint8_t>();
    p.data = r.list<uint8_t>();
    p.cbufTypes = r.list<uint8_t>();
    p.gprs = r.value<uint8_t>();
    p.boundTextures = r.value<uint8_t>();
    return p;
}

bool readFile(const std::string &path, std::string &out)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && uint64_t(st.st_size) <= kSizeLimit;
    if (ok) {
        out.resize(size_t(st.st_size));
        size_t at = 0;
        while (ok && at < out.size()) {
            const ssize_t n = read(fd, &out[at], out.size() - at);
            ok = n > 0;
            at += ok ? size_t(n) : 0;
        }
    }
    close(fd);
    return ok;
}

bool writeAll(int fd, const std::string &bytes)
{
    for (size_t at = 0; at < bytes.size();) {
        const ssize_t n = write(fd, bytes.data() + at, bytes.size() - at);
        if (n <= 0)
            return false;
        at += size_t(n);
    }
    return true;
}

struct File {
    std::string path;
    uint64_t bytes;
    time_t mtime;
};

std::vector<File> files(const std::string &dir, const char *suffix)
{
    std::vector<File> out;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return out;
    while (const dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        const bool match =
            name.size() > strlen(suffix) && !name.compare(name.size() - strlen(suffix), std::string::npos, suffix);
        struct stat st;
        const std::string path = dir + "/" + name;
        if (match && lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            out.push_back({path, uint64_t(st.st_size), st.st_mtime});
    }
    closedir(d);
    return out;
}

// Another build's entries (raw-<16 hex digits> but ours) and what a writer that died left half written
void removeStale(const std::string &parent, const std::string &ours)
{
    if (DIR *d = opendir(parent.c_str())) {
        std::vector<std::string> stale;
        while (const dirent *e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() == 20 && !name.compare(0, 4, "raw-") && name != ours &&
                name.find_first_not_of("0123456789abcdef", 4) == std::string::npos)
                stale.push_back(parent + "/" + name);
        }
        closedir(d);
        for (const std::string &dir : stale) {
            for (const char *kind : {".rec", ".tmp"})
                for (const File &f : files(dir, kind))
                    unlink(f.path.c_str());
            rmdir(dir.c_str());
        }
    }
    const time_t now = time(nullptr);
    for (const File &f : files(parent + "/" + ours, ".tmp"))
        if (now - f.mtime > kStaleTemporarySeconds)
            unlink(f.path.c_str());
}

}  // namespace

uint64_t CacheKey::name() const
{
    uint64_t h = Pack::fnv(&stack, sizeof stack);
    std::string all(reinterpret_cast<const char *>(&h), sizeof h);
    all.append(reinterpret_cast<const char *>(vertexAir), sizeof vertexAir);
    all.append(reinterpret_cast<const char *>(fragmentAir), sizeof fragmentAir);
    all += key;
    return Pack::fnv(all.data(), all.size());
}

void writeEntryHead(Writer &w, const CacheKey &k, uint32_t kind)
{
    w.bytes(kEntryMagic, sizeof kEntryMagic);
    w.value(kEntryFormat);
    w.value(kind);
    w.value(k.stack);
    w.bytes(k.vertexAir, sizeof k.vertexAir);
    w.bytes(k.fragmentAir, sizeof k.fragmentAir);
    w.text(k.key);
}

bool readEntryHead(const std::string &bytes, Reader &r, const CacheKey &k, uint32_t &kind)
{
    if (bytes.size() < sizeof kEntryMagic + 8)
        return false;
    uint64_t sum;
    memcpy(&sum, bytes.data() + bytes.size() - 8, 8);
    if (sum != Pack::fnv(bytes.data(), bytes.size() - 8))
        return false;
    char magic[sizeof kEntryMagic];
    uint8_t vertexAir[32], fragmentAir[32];
    r.bytes(magic, sizeof magic);
    const uint32_t format = r.value<uint32_t>();
    kind = r.value<uint32_t>();
    const uint64_t stack = r.value<uint64_t>();
    r.bytes(vertexAir, sizeof vertexAir);
    r.bytes(fragmentAir, sizeof fragmentAir);
    const std::string key = r.text();
    return r.ok() && !memcmp(magic, kEntryMagic, sizeof kEntryMagic) && format == kEntryFormat && stack == k.stack &&
           !memcmp(vertexAir, k.vertexAir, sizeof vertexAir) &&
           !memcmp(fragmentAir, k.fragmentAir, sizeof fragmentAir) && key == k.key;
}

namespace {

// A record's body in an entry, its stride twin's after it
void writeRecord(Writer &w, const RuntimeRecord *record)
{
    w.value(record->pipeline);
    w.text(record->keyString);
    writeProgram(w, record->programs[0]);
    writeProgram(w, record->programs[1]);
    if (tessellates(record->pipeline)) {
        writeProgram(w, record->tessPrograms[0]);
        writeProgram(w, record->tessPrograms[1]);
    }
    w.list(record->blob);
    w.list(record->extras);
    w.list(record->restore);
    w.list(record->samplers);
    w.value(record->amplificationLayerWord);
    w.value(record->amplificationViewportWord);
    w.value(uint8_t((record->splitTried ? kSplitTried : 0) | (record->split ? kSplit : 0)));
    if (record->split) {
        w.value(record->splitRecord);
        writeProgram(w, record->splitPrograms[0]);
        writeProgram(w, record->splitPrograms[1]);
    }
    w.value(uint32_t(record->variants.size()));
    for (const RuntimeRecord::Variant &v : record->variants) {
        w.value(v.assume);
        writeProgram(w, v.program);
    }
    w.list(record->spill);
    w.value(uint8_t(record->strideTwin != nullptr));
    if (record->strideTwin)
        writeRecord(w, record->strideTwin.get());
}

// The record writeRecord wrote; a stride twin only where `twinned` (a twin has none)
bool readRecord(Reader &r, RuntimeRecord &rec, bool twinned)
{
    rec.pipeline = r.value<PackPipeline>();
    rec.keyString = r.text();
    rec.programs[0] = readProgram(r);
    rec.programs[1] = readProgram(r);
    if (tessellates(rec.pipeline)) {
        rec.tessPrograms[0] = readProgram(r);
        rec.tessPrograms[1] = readProgram(r);
    }
    rec.blob = r.list<uint32_t>();
    rec.extras = r.list<std::pair<uint32_t, uint32_t>>();
    rec.restore = r.list<std::pair<uint32_t, uint32_t>>();
    rec.samplers = r.list<std::array<uint32_t, 8>>();
    rec.amplificationLayerWord = r.value<uint8_t>();
    rec.amplificationViewportWord = r.value<uint8_t>();
    const uint8_t split = r.value<uint8_t>();
    rec.splitTried = split & kSplitTried;
    rec.split = split & kSplit;
    if (rec.split) {
        rec.splitRecord = r.value<PackSplit>();
        rec.splitPrograms[0] = readProgram(r);
        rec.splitPrograms[1] = readProgram(r);
    }
    const uint32_t variants = r.value<uint32_t>();
    for (uint32_t i = 0; r.ok() && i < variants && i < 2u * 255; i++) {
        RuntimeRecord::Variant &v = rec.variants.emplace_back();
        v.assume = r.value<uint32_t>();
        v.program = readProgram(r);
    }
    if (rec.variants.size() != variants || variants != 0u + rec.pipeline.variantCount[0] + rec.pipeline.variantCount[1])
        return false;
    rec.spill = r.list<uint32_t>();
    if (rec.spill.size() != 0u + rec.pipeline.spillWords[0] + rec.pipeline.spillWords[1])
        return false;
    if (r.value<uint8_t>()) {
        auto twin = std::make_shared<RuntimeRecord>();
        if (!twinned || !readRecord(r, *twin, false))
            return false;
        rec.strideTwin = std::move(twin);
    }
    return r.ok();
}

}  // namespace

std::string encodeEntry(const CacheKey &k, const RuntimeRecord *record, const std::string &refusal)
{
    Writer w;
    writeEntryHead(w, k, record ? kKindRecord : kKindRefusal);
    if (record)
        writeRecord(w, record);
    else
        w.text(refusal);
    w.value(Pack::fnv(w.out.data(), w.out.size()));
    return w.out;
}

RecordCache::Found decodeEntry(const std::string &bytes, const CacheKey &k, RuntimeRecord &record, std::string &refusal)
{
    Reader r(bytes);
    uint32_t kind = 0;
    if (!readEntryHead(bytes, r, k, kind))
        return RecordCache::Found::None;
    if (kind == kKindRefusal) {
        refusal = r.text();
        return r.ok() && r.at() == bytes.size() - 8 ? RecordCache::Found::Refusal : RecordCache::Found::None;
    }
    if (kind != kKindRecord)
        return RecordCache::Found::None;
    RuntimeRecord rec;
    if (!readRecord(r, rec, true) || r.at() != bytes.size() - 8)
        return RecordCache::Found::None;
    record = std::move(rec);
    return RecordCache::Found::Record;
}

RecordCache::RecordCache() = default;
RecordCache::~RecordCache() = default;

bool RecordCache::open(const std::string &dir, uint64_t stack, uint64_t capBytes)
{
    if (!attach(dir, stack, capBytes))
        return false;
    tidy();
    return true;
}

bool RecordCache::attach(const std::string &dir, uint64_t stack, uint64_t capBytes)
{
    std::lock_guard<std::mutex> l(lock_);
    const std::string mine = dir + "/raw-" + hex16(stack);
    if (dir.empty() || (mkdir(mine.c_str(), 0700) != 0 && errno != EEXIST) || access(mine.c_str(), W_OK) != 0)
        return false;
    parent_ = dir;
    dir_ = mine;
    if (stack_ != stack)
        seed_.reset();
    stack_ = stack;
    cap_ = capBytes;
    bytes_ = 0;
    return true;
}

bool RecordCache::attachSeed(const std::string &dir, uint64_t stack)
{
    std::lock_guard<std::mutex> l(lock_);
    if (dir.empty() || (!dir_.empty() && stack_ != stack))
        return false;
    uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
    memcpy(identity, &stack, sizeof stack);
    auto seed = std::make_unique<switch_seed_pack::Seed>();
    if (!seed->open(dir + "/raw-" + hex16(stack), sizeof(uint64_t), identity))
        return false;
    stack_ = stack;
    seed_ = std::move(seed);
    return true;
}

const uint8_t *RecordCache::findSeedSlot(const uint8_t *slot, size_t size, const uint8_t *&mapping,
                                         uint64_t &bytes) const
{
    const uint64_t value = seedSlotName(slot, size);
    uint8_t name[sizeof value];
    for (unsigned i = 0; i < sizeof name; ++i)
        name[i] = uint8_t(value >> (8 * (sizeof name - 1 - i)));
    switch_seed_pack::Bytes found, pack;
    if (!seed_ || !seed_->find(name, found) || found.size < size || memcmp(found.data, slot, size) ||
        !seed_->packOf(found.data, pack))
        return nullptr;
    mapping = pack.data;
    bytes = pack.size;
    return found.data;
}

void RecordCache::tidy()
{
    if (dir_.empty())
        return;
    removeStale(parent_, "raw-" + hex16(stack_));
    std::lock_guard<std::mutex> l(lock_);  // background stores cannot race the directory accounting snapshot
    uint64_t bytes = 0;
    for (const File &f : files(dir_, ".rec"))
        bytes += f.bytes;
    bytes_ = bytes;
}

std::string RecordCache::path(const CacheKey &k) const
{
    return dir_ + "/" + hex16(k.name()) + ".rec";
}

bool RecordCache::loadBytes(bool seed, const CacheKey &k, std::string &bytes) const
{
    if (k.stack != stack_)
        return false;
    if (!seed)
        return !dir_.empty() && readFile(path(k), bytes);
    const uint64_t value = k.name();
    uint8_t name[sizeof value];
    for (unsigned i = 0; i < sizeof name; ++i)
        name[i] = uint8_t(value >> (8 * (sizeof name - 1 - i)));
    switch_seed_pack::Bytes found;
    if (!seed_ || !seed_->find(name, found) || found.size > kSizeLimit)
        return false;
    bytes.assign(reinterpret_cast<const char *>(found.data), found.size);
    return true;
}

RecordCache::Found RecordCache::load(const CacheKey &k, RuntimeRecord &record, std::string &refusal, bool *seeded)
{
    std::string bytes;
    for (const bool seed : {true, false})
        if (loadBytes(seed, k, bytes))
            if (const Found found = decodeEntry(bytes, k, record, refusal); found != Found::None) {
                if (seeded)
                    *seeded = seed;
                return found;
            }
    return Found::None;
}

RecordCache::Found RecordCache::loadKernel(const CacheKey &k, ComputeRecord &record, std::string &refusal)
{
    std::string bytes;
    for (const bool seed : {true, false})
        if (loadBytes(seed, k, bytes))
            if (const Found found = decodeKernelEntry(bytes, k, record, refusal); found != Found::None)
                return found;
    return Found::None;
}

bool RecordCache::store(const CacheKey &k, const RuntimeRecord *record, const std::string &refusal)
{
    return dir_.empty() || k.stack != stack_ ? false : storeBytes(k, encodeEntry(k, record, refusal));
}

bool RecordCache::storeKernel(const CacheKey &k, const ComputeRecord *record, const std::string &refusal)
{
    return dir_.empty() || k.stack != stack_ ? false : storeBytes(k, encodeKernelEntry(k, record, refusal));
}

bool RecordCache::storeBytes(const CacheKey &k, const std::string &bytes)
{
    static std::atomic<uint32_t> serial{0};
    const std::string final = path(k);
    std::lock_guard<std::mutex> l(lock_);
    if (cap_ && bytes.size() > cap_ / 4 * 3)
        return false;
    if (cap_ && bytes_ + bytes.size() > cap_)
        evict(bytes.size());
    const std::string temporary = dir_ + "/" + std::to_string(getpid()) + "-" + std::to_string(serial++) + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    const bool written = writeAll(fd, bytes);
    close(fd);
    struct stat old;
    const uint64_t replaced = stat(final.c_str(), &old) == 0 ? uint64_t(old.st_size) : 0;
    if (!written || rename(temporary.c_str(), final.c_str()) != 0) {
        unlink(temporary.c_str());
        return false;
    }
    bytes_ = bytes_ - std::min(bytes_, replaced) + bytes.size();
    return true;
}

// lock_ held: the oldest entries removed until `room` more bytes leave the cache at three quarters of its cap
void RecordCache::evict(uint64_t room)
{
    std::vector<File> all = files(dir_, ".rec");
    std::sort(all.begin(), all.end(), [](const File &a, const File &b) {
        return a.mtime < b.mtime;
    });
    bytes_ = 0;
    for (const File &f : all)
        bytes_ += f.bytes;
    for (const File &f : all) {
        if (bytes_ + room <= cap_ / 4 * 3)
            break;
        if (unlink(f.path.c_str()) == 0)
            bytes_ -= f.bytes;
    }
}

}  // namespace raw
