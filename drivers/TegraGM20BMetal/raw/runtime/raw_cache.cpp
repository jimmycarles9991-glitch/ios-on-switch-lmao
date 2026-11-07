// SPDX-License-Identifier: MIT
// See raw_cache.h.
#include "raw_cache.h"

#include <dirent.h>
#include <dispatch/dispatch.h>
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

constexpr char kMagic[8] = {'S', 'W', 'R', 'A', 'W', 'R', 'E', 'C'};
constexpr uint32_t kFormat = 1;
constexpr uint32_t kKindRecord = 1, kKindRefusal = 2;
constexpr uint64_t kSizeLimit = 1u << 24;  // an entry larger than any record could be: a broken file
constexpr time_t kStaleTemporarySeconds = 60;

std::string hex16(uint64_t v)
{
    char b[17];
    snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

class Writer {
public:
    std::string out;

    void bytes(const void *p, size_t n)
    {
        out.append(static_cast<const char *>(p), n);
    }

    template <class T> void value(T v)
    {
        bytes(&v, sizeof v);
    }

    void text(const std::string &s)
    {
        value(uint32_t(s.size()));
        bytes(s.data(), s.size());
    }

    template <class T> void list(const std::vector<T> &v)
    {
        value(uint32_t(v.size()));
        bytes(v.data(), v.size() * sizeof(T));
    }
};

class Reader {
public:
    explicit Reader(const std::string &in) : in_(in)
    {}

    bool bytes(void *p, size_t n)
    {
        if (n > in_.size() - at_)
            return ok_ = false;
        memcpy(p, in_.data() + at_, n);
        at_ += n;
        return true;
    }

    template <class T> T value()
    {
        T v{};
        bytes(&v, sizeof v);
        return v;
    }

    std::string text()
    {
        const uint32_t n = value<uint32_t>();
        if (!ok_ || n > in_.size() - at_)
            return ok_ = false, std::string();
        std::string s = in_.substr(at_, n);
        at_ += n;
        return s;
    }

    template <class T> std::vector<T> list()
    {
        const uint32_t n = value<uint32_t>();
        if (!ok_ || n > (in_.size() - at_) / sizeof(T))
            return ok_ = false, std::vector<T>();
        std::vector<T> v(n);
        bytes(v.data(), n * sizeof(T));
        return v;
    }

    bool ok() const
    {
        return ok_;
    }

    size_t at() const
    {
        return at_;
    }

private:
    const std::string &in_;
    size_t at_ = 0;
    bool ok_ = true;
};

void writeProgram(Writer &w, const CapturedProgram &p)
{
    w.text(p.id);
    w.list(p.image);
    w.list(p.data);
    w.list(p.cbufTypes);
}

CapturedProgram readProgram(Reader &r)
{
    CapturedProgram p;
    p.id = r.text();
    p.image = r.list<uint8_t>();
    p.data = r.list<uint8_t>();
    p.cbufTypes = r.list<uint8_t>();
    return p;
}

bool readFile(const std::string &path, std::string &out)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && uint64_t(st.st_size) <= kSizeLimit;
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

uint64_t nowUs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
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

std::string encodeEntry(const CacheKey &k, const RuntimeRecord *record, const std::string &refusal)
{
    Writer w;
    w.bytes(kMagic, sizeof kMagic);
    w.value(kFormat);
    w.value(record ? kKindRecord : kKindRefusal);
    w.value(k.stack);
    w.bytes(k.vertexAir, sizeof k.vertexAir);
    w.bytes(k.fragmentAir, sizeof k.fragmentAir);
    w.text(k.key);
    if (record) {
        w.value(record->pipeline);
        w.text(record->keyString);
        writeProgram(w, record->programs[0]);
        writeProgram(w, record->programs[1]);
        w.list(record->blob);
        w.list(record->extras);
        w.list(record->restore);
        w.list(record->samplers);
    } else {
        w.text(refusal);
    }
    w.value(Pack::fnv(w.out.data(), w.out.size()));
    return w.out;
}

RecordCache::Found decodeEntry(const std::string &bytes, const CacheKey &k, RuntimeRecord &record, std::string &refusal)
{
    if (bytes.size() < sizeof kMagic + 8)
        return RecordCache::Found::None;
    uint64_t sum;
    memcpy(&sum, bytes.data() + bytes.size() - 8, 8);
    if (sum != Pack::fnv(bytes.data(), bytes.size() - 8))
        return RecordCache::Found::None;
    Reader r(bytes);
    char magic[sizeof kMagic];
    uint8_t vertexAir[32], fragmentAir[32];
    r.bytes(magic, sizeof magic);
    const uint32_t format = r.value<uint32_t>(), kind = r.value<uint32_t>();
    const uint64_t stack = r.value<uint64_t>();
    r.bytes(vertexAir, sizeof vertexAir);
    r.bytes(fragmentAir, sizeof fragmentAir);
    const std::string key = r.text();
    if (!r.ok() || memcmp(magic, kMagic, sizeof kMagic) || format != kFormat || stack != k.stack ||
        memcmp(vertexAir, k.vertexAir, sizeof vertexAir) || memcmp(fragmentAir, k.fragmentAir, sizeof fragmentAir) ||
        key != k.key)
        return RecordCache::Found::None;
    if (kind == kKindRefusal) {
        refusal = r.text();
        return r.ok() && r.at() == bytes.size() - 8 ? RecordCache::Found::Refusal : RecordCache::Found::None;
    }
    if (kind != kKindRecord)
        return RecordCache::Found::None;
    RuntimeRecord rec;
    rec.pipeline = r.value<PackPipeline>();
    rec.keyString = r.text();
    rec.programs[0] = readProgram(r);
    rec.programs[1] = readProgram(r);
    rec.blob = r.list<uint32_t>();
    rec.extras = r.list<std::pair<uint32_t, uint32_t>>();
    rec.restore = r.list<std::pair<uint32_t, uint32_t>>();
    rec.samplers = r.list<std::array<uint32_t, 8>>();
    if (!r.ok() || r.at() != bytes.size() - 8)
        return RecordCache::Found::None;
    record = std::move(rec);
    return RecordCache::Found::Record;
}

bool RecordCache::open(const std::string &dir, uint64_t stack, uint64_t capBytes)
{
    std::lock_guard<std::mutex> l(lock_);
    const std::string name = "raw-" + hex16(stack);
    const std::string mine = dir + "/" + name;
    if (dir.empty() || (mkdir(mine.c_str(), 0700) != 0 && errno != EEXIST) || access(mine.c_str(), W_OK) != 0)
        return false;
    removeStale(dir, name);
    dir_ = mine;
    stack_ = stack;
    cap_ = capBytes;
    bytes_ = 0;
    for (const File &f : files(dir_, ".rec"))
        bytes_ += f.bytes;
    return true;
}

std::string RecordCache::path(const CacheKey &k) const
{
    return dir_ + "/" + hex16(k.name()) + ".rec";
}

RecordCache::Found RecordCache::load(const CacheKey &k, RuntimeRecord &record, std::string &refusal)
{
    if (dir_.empty() || k.stack != stack_)
        return Found::None;
    std::string bytes;
    if (!readFile(path(k), bytes))
        return Found::None;
    return decodeEntry(bytes, k, record, refusal);
}

bool RecordCache::store(const CacheKey &k, const RuntimeRecord *record, const std::string &refusal)
{
    static std::atomic<uint32_t> serial{0};
    if (dir_.empty() || k.stack != stack_)
        return false;
    const std::string bytes = encodeEntry(k, record, refusal), final = path(k);
    std::lock_guard<std::mutex> l(lock_);
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

struct Compiler::Job {
    Compiler *compiler;
    CacheKey key;
    Done done;
};

Compiler::Compiler(RecordCache *cache, Make make)
    : cache_(cache), make_(std::move(make)),
      queue_(dispatch_queue_create(
          "com.brunostjohn.switch.raw.compile",
          dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_BACKGROUND, 0)))
{}

Compiler::~Compiler()
{
    drain();
    dispatch_release(static_cast<dispatch_queue_t>(queue_));
}

Compiler::State Compiler::ask(const CacheKey &k, Done done, const RuntimeRecord **record)
{
    std::lock_guard<std::mutex> l(lock_);
    const auto [it, first] = entries_.try_emplace(k.name());
    if (!first) {
        if (record)
            *record = it->second.record.get();
        return it->second.state;
    }
    queued_++;
    dispatch_async_f(static_cast<dispatch_queue_t>(queue_), new Job{this, k, std::move(done)}, run);
    return State::Pending;
}

void Compiler::run(void *context)
{
    std::unique_ptr<Job> job(static_cast<Job *>(context));
    Compiler &c = *job->compiler;
    const uint64_t began = nowUs();
    auto record = std::make_unique<RuntimeRecord>();
    std::string why;
    const RecordCache::Found found = c.cache_ ? c.cache_->load(job->key, *record, why) : RecordCache::Found::None;
    Outcome outcome = found == RecordCache::Found::Record    ? Outcome::Made
                      : found == RecordCache::Found::Refusal ? Outcome::Refused
                                                             : c.make_(job->key, *record, why);
    if (found == RecordCache::Found::None && outcome != Outcome::Failed && c.cache_)
        c.cache_->store(job->key, outcome == Outcome::Made ? record.get() : nullptr, why);
    const RuntimeRecord *made = nullptr;
    {
        std::lock_guard<std::mutex> l(c.lock_);
        Entry &e = c.entries_[job->key.name()];
        e.why = why;
        if (outcome == Outcome::Made) {
            e.record = std::move(record);
            made = e.record.get();
        }
        (found == RecordCache::Found::None ? c.stats_.misses : c.stats_.hits)++;
        c.stats_.refusals += outcome == Outcome::Refused;
        c.stats_.failures += outcome == Outcome::Failed;
        c.stats_.makeUs += nowUs() - began;
        e.state = outcome == Outcome::Made ? State::Ready : State::Refused;
    }
    if (job->done)
        job->done(made, why);
    std::lock_guard<std::mutex> l(c.lock_);
    c.queued_--;
    c.idle_.notify_all();
}

void Compiler::drain()
{
    std::unique_lock<std::mutex> l(lock_);
    idle_.wait(l, [&] {
        return queued_ == 0;
    });
}

Compiler::Stats Compiler::stats() const
{
    std::lock_guard<std::mutex> l(lock_);
    return stats_;
}

}  // namespace raw
