// SPDX-License-Identifier: MIT
// See gm20b_cache.h.
#include "gm20b_cache.h"
#include "cache_io.h"
#include "../../kits/SwitchSeedPack/SwitchSeedPack.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace gm20b {
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
    w.list(p.farUbos);
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
    p.farUbos = r.list<uint16_t>();
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
            if (name.size() == 20 && !name.compare(0, 4, "rec-") && name != ours &&
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

static uint64_t fnvStep(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

uint64_t CacheKey::name() const
{
    const uint64_t stackHash = Pack::fnv(&stack, sizeof stack);
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnvStep(h, &stackHash, sizeof stackHash);
    h = fnvStep(h, vertexAir, sizeof vertexAir);
    h = fnvStep(h, fragmentAir, sizeof fragmentAir);
    return fnvStep(h, key.data(), key.size());
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

bool readEntryHead(std::string_view bytes, Reader &r, const CacheKey &k, uint32_t &kind)
{
    if (bytes.size() < sizeof kEntryMagic)
        return false;
    char magic[sizeof kEntryMagic];
    uint8_t vertexAir[32], fragmentAir[32];
    r.bytes(magic, sizeof magic);
    const uint32_t format = r.value<uint32_t>();
    kind = r.value<uint32_t>();
    const uint64_t stack = r.value<uint64_t>();
    r.bytes(vertexAir, sizeof vertexAir);
    r.bytes(fragmentAir, sizeof fragmentAir);
    const std::string_view key = r.textView();
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
    w.list(record->far);
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
    rec.far = r.list<uint32_t>();
    if (entryCount(rec.pipeline) > kSetEntriesMax || rec.far.size() != farWords(entryCount(rec.pipeline)) ||
        entriesRefusal(rec.pipeline, rec.far.empty() ? nullptr : rec.far.data(), uint32_t(rec.samplers.size())))
        return false;
    for (const CapturedProgram *p : {&rec.programs[0], &rec.programs[1], &rec.tessPrograms[0], &rec.tessPrograms[1]})
        if (!p->farUbos.empty() && p->farUbos.size() != p->cbufTypes.size())
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
    return w.out;
}

RecordCache::Found decodeEntry(std::string_view bytes, const CacheKey &k, RuntimeRecord &record, std::string &refusal)
{
    Reader r(bytes);
    uint32_t kind = 0;
    if (!readEntryHead(bytes, r, k, kind))
        return RecordCache::Found::None;
    if (kind == kKindRefusal) {
        refusal = r.text();
        return r.ok() && r.at() == bytes.size() ? RecordCache::Found::Refusal : RecordCache::Found::None;
    }
    if (kind != kKindRecord)
        return RecordCache::Found::None;
    RuntimeRecord rec;
    if (!readRecord(r, rec, true) || r.at() != bytes.size())
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

bool RecordCache::attach(const std::string &dir, uint64_t stack, uint64_t capBytes, bool exclusive)
{
    std::lock_guard<std::mutex> l(lock_);
    const std::string mine = dir + "/rec-" + hex16(stack);
    if (dir.empty() || (mkdir(mine.c_str(), 0700) != 0 && errno != EEXIST) || access(mine.c_str(), W_OK) != 0)
        return false;
    dirReady_.store(false, std::memory_order_release);
    parent_ = dir;
    dir_ = mine;
    if (seed_ && seedStack_ != stack)
        seed_.reset();
    stack_ = stack;
    cap_ = capBytes;
    bytes_ = 0;
    exclusive_ = exclusive;
    anyStored_ = false;
    diskMayHaveEntries_.store(true, std::memory_order_release);
    dirReady_.store(true, std::memory_order_release);
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
    if (!seed->open(dir + "/rec-" + hex16(stack), sizeof(uint64_t), identity))
        return false;
    seedStack_ = stack;
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
    std::string parent, mine;
    {
        std::lock_guard<std::mutex> l(lock_);
        if (dir_.empty())
            return;
        parent = parent_;
        mine = "rec-" + hex16(stack_);
    }
    removeStale(parent, mine);
    std::lock_guard<std::mutex> l(lock_);  // background stores cannot race the directory accounting snapshot
    if (dir_.empty())
        return;
    uint64_t bytes = 0;
    for (const File &f : files(dir_, ".rec"))
        bytes += f.bytes;
    bytes_ = bytes;
    diskMayHaveEntries_.store(!exclusive_ || bytes != 0 || anyStored_, std::memory_order_release);
}

std::string RecordCache::path(const CacheKey &k) const
{
    return dir_ + "/" + hex16(k.name()) + ".rec";
}

bool RecordCache::findSeedBytes(const CacheKey &k, std::string_view &bytes) const
{
    if (!seed_ || k.stack != seedStack_)
        return false;
    const uint64_t value = k.name();
    uint8_t name[sizeof value];
    for (unsigned i = 0; i < sizeof name; ++i)
        name[i] = uint8_t(value >> (8 * (sizeof name - 1 - i)));
    switch_seed_pack::Bytes found;
    if (!seed_->find(name, found) || found.size > kSizeLimit)
        return false;
    bytes = std::string_view(reinterpret_cast<const char *>(found.data), found.size);
    return true;
}

bool RecordCache::loadDiskBytes(const CacheKey &k, std::string &bytes) const
{
    if (!dirReady_.load(std::memory_order_acquire) || !diskMayHaveEntries_.load(std::memory_order_acquire))
        return false;
    std::string file;
    {
        std::lock_guard<std::mutex> l(lock_);
        if (dir_.empty() || k.stack != stack_)
            return false;
        file = path(k);
    }
    return readFile(file, bytes);
}

RecordCache::Found RecordCache::load(const CacheKey &k, RuntimeRecord &record, std::string &refusal, bool *seeded)
{
    std::string_view seedView;
    if (findSeedBytes(k, seedView))
        if (const Found found = decodeEntry(seedView, k, record, refusal); found != Found::None) {
            if (seeded)
                *seeded = true;
            return found;
        }
    std::string bytes;
    if (loadDiskBytes(k, bytes))
        if (const Found found = decodeEntry(bytes, k, record, refusal); found != Found::None) {
            if (seeded)
                *seeded = false;
            return found;
        }
    return Found::None;
}

RecordCache::Found RecordCache::loadKernel(const CacheKey &k, ComputeRecord &record, std::string &refusal)
{
    std::string_view seedView;
    if (findSeedBytes(k, seedView))
        if (const Found found = decodeKernelEntry(seedView, k, record, refusal); found != Found::None)
            return found;
    std::string bytes;
    if (loadDiskBytes(k, bytes))
        if (const Found found = decodeKernelEntry(bytes, k, record, refusal); found != Found::None)
            return found;
    return Found::None;
}

bool RecordCache::store(const CacheKey &k, const RuntimeRecord *record, const std::string &refusal)
{
    return dirReady_.load(std::memory_order_acquire) ? storeBytes(k, encodeEntry(k, record, refusal)) : false;
}

bool RecordCache::storeKernel(const CacheKey &k, const ComputeRecord *record, const std::string &refusal)
{
    return dirReady_.load(std::memory_order_acquire) ? storeBytes(k, encodeKernelEntry(k, record, refusal)) : false;
}

bool RecordCache::storeBytes(const CacheKey &k, const std::string &bytes)
{
    static std::atomic<uint32_t> serial{0};
    std::lock_guard<std::mutex> l(lock_);
    if (dir_.empty() || k.stack != stack_)
        return false;
    const std::string final = path(k);
    if (cap_ && bytes.size() > cap_ / 4 * 3)
        return false;
    if (cap_ && bytes_ + bytes.size() > cap_)
        evict(bytes.size());
    const std::string temporary = dir_ + "/" + std::to_string(getpid()) + "-" +
                                  std::to_string(serial.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    // synced before it is renamed in: nothing reads an entry's bytes again to find it whole after a crash
    const bool written = writeAll(fd, bytes) && fsync(fd) == 0;
    close(fd);
    struct stat old;
    const uint64_t replaced = stat(final.c_str(), &old) == 0 ? uint64_t(old.st_size) : 0;
    if (!written || rename(temporary.c_str(), final.c_str()) != 0) {
        unlink(temporary.c_str());
        return false;
    }
    bytes_ = bytes_ - std::min(bytes_, replaced) + bytes.size();
    anyStored_ = true;
    diskMayHaveEntries_.store(true, std::memory_order_release);
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

}  // namespace gm20b
