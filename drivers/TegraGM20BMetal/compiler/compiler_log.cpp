// SPDX-License-Identifier: MIT
// See compiler_log.h.
#include "compiler_log.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"

namespace switch_compiler {
namespace {

constexpr char kMagic[8] = {'S', 'W', 'C', 'L', 'O', 'G', '0', '2'};
constexpr uint32_t kRecordMark = 0x51455257;  // "WREQ"
constexpr size_t kDurableAt = 8, kBootAt = 16;
static_assert(kBootAt + switch_seed_pack::kStoreBootBytes <= RequestLog::kHeaderBytes, "the log's header");

bool writeAll(int fd, const void *bytes, size_t size)
{
    for (size_t at = 0; at < size;) {
        const ssize_t wrote = write(fd, static_cast<const uint8_t *>(bytes) + at, size - at);
        if (wrote <= 0)
            return false;
        at += size_t(wrote);
    }
    return true;
}

bool readSpan(int fd, const RequestLog::Span &s, std::string &out)
{
    out.resize(s.length);
    return pread(fd, out.data(), s.length, off_t(s.at)) == ssize_t(s.length);
}

bool append(int fd, const std::string &request)
{
    const uint32_t head[2] = {uint32_t(request.size()), kRecordMark};
    return writeAll(fd, head, sizeof head) && writeAll(fd, request.data(), request.size());
}

bool writeHeader(int fd, uint64_t durable, const char *boot)
{
    uint8_t header[RequestLog::kHeaderBytes] = {};
    memcpy(header, kMagic, sizeof kMagic);
    memcpy(header + kDurableAt, &durable, sizeof durable);
    memcpy(header + kBootAt, boot, switch_seed_pack::kStoreBootBytes);
    return pwrite(fd, header, sizeof header, 0) == ssize_t(sizeof header);
}

// Where each request between `from` and `limit` is, and the end of the last: a head that does not fit ends it. `marked`:
// heads carry kRecordMark (a log of the first layout carries a checksum there, which nothing reads)
std::vector<RequestLog::Span> scan(int fd, uint64_t from, uint64_t limit, bool marked, uint64_t &good)
{
    std::vector<RequestLog::Span> out;
    good = from;
    limit = std::min(limit, RequestLog::kLogBytes);
    for (uint64_t at = from; at + 8 <= limit;) {
        uint32_t head[2];
        if (pread(fd, head, sizeof head, off_t(at)) != ssize_t(sizeof head) || head[0] > RequestLog::kRequestBytes ||
            head[0] > limit - at - 8 || (marked && head[1] != kRecordMark))
            break;
        out.push_back({at + 8, head[0]});
        at += 8 + head[0];
        good = at;
    }
    return out;
}

}  // namespace

bool RequestLog::open(const std::string &path, const char *boot)
{
    static_assert(sizeof boot_ == switch_seed_pack::kStoreBootBytes, "a boot as the store names it");
    std::lock_guard<std::mutex> l(lock_);
    path_ = path;
    bool known = false;
    if (boot) {
        memcpy(boot_, boot, strnlen(boot, sizeof boot_ - 1));
        known = boot_[0] != 0;
    } else {
        known = switch_seed_pack::currentBoot(boot_);
    }
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    struct stat info = {};
    if (fd_ < 0 || fstat(fd_, &info) || !S_ISREG(info.st_mode)) {
        if (fd_ >= 0)
            close(fd_);
        fd_ = -1;
        return false;
    }
    const uint64_t size = uint64_t(info.st_size);
    uint8_t header[kHeaderBytes] = {};
    const bool ours = size >= kHeaderBytes && pread(fd_, header, sizeof header, 0) == ssize_t(sizeof header) &&
                      memcmp(header, kMagic, sizeof kMagic) == 0;
    bool written = true;
    if (ours) {
        memcpy(&durable_, header + kDurableAt, sizeof durable_);
        const bool sameBoot = known && memcmp(header + kBootAt, boot_, sizeof boot_) == 0;
        read_ = scan(fd_, kHeaderBytes, sameBoot ? size : std::min(size, durable_), true, bytes_);
        durable_ = std::min(durable_, bytes_);
        written = ftruncate(fd_, off_t(bytes_)) == 0 && writeHeader(fd_, durable_, boot_);
    } else if (size != 0) {
        uint64_t end = 0;
        const std::vector<Span> legacy = scan(fd_, 0, size, false, end);
        written = rewriteLocked(legacy, &read_);
    } else {
        bytes_ = durable_ = kHeaderBytes;
        written = writeHeader(fd_, durable_, boot_);
    }
    if (!written || lseek(fd_, 0, SEEK_END) < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

bool RequestLog::persist()
{
    int fd = -1;
    uint64_t end = 0;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> l(lock_);
        if (fd_ < 0 || bytes_ == durable_)
            return true;
        fd = dup(fd_);
        end = bytes_;
        generation = generation_;
    }
    if (fd < 0)
        return false;
    bool synced = switch_seed_pack::syncBarrier(fd);
    if (synced) {
        std::lock_guard<std::mutex> l(lock_);
        // a log written again meanwhile was synced whole before it was renamed in
        if (generation == generation_ && end > durable_) {
            durable_ = end;
            synced = pwrite(fd, &end, sizeof end, off_t(kDurableAt)) == ssize_t(sizeof end);
        }
    }
    synced = synced && switch_seed_pack::syncBarrier(fd);
    close(fd);
    return synced;
}

// lock held: `spans` of the open file (in its order) written as the log, each read on its own, and synced before it is
// renamed in; the file it replaces closed. `moved`: where each span is in the new file. False: the log is as it was
bool RequestLog::rewriteLocked(const std::vector<Span> &spans, std::vector<Span> *moved)
{
    const std::string fresh = path_ + ".new";
    const int fd = ::open(fresh.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    bool written = fd >= 0 && lseek(fd, off_t(kHeaderBytes), SEEK_SET) == off_t(kHeaderBytes);
    uint64_t bytes = kHeaderBytes;
    std::vector<Span> at;
    std::string request;
    at.reserve(spans.size());
    for (const Span &s : spans) {
        written = written && readSpan(fd_, s, request) && append(fd, request);
        if (!written)
            break;
        at.push_back({bytes + 8, s.length});
        bytes += 8 + s.length;
    }
    written = written && writeHeader(fd, bytes, boot_) && switch_seed_pack::syncBarrier(fd);
    if (!written || rename(fresh.c_str(), path_.c_str())) {
        if (fd >= 0)
            close(fd);
        unlink(fresh.c_str());
        return false;
    }
    close(fd_);
    fd_ = fd;
    bytes_ = durable_ = bytes;
    generation_++;
    if (moved)
        *moved = std::move(at);
    return true;
}

// lock held: the log written again without its oldest requests, so `room` more bytes fit in three quarters of it. Before
// the replay, what it replays is then what the log kept, added since or not
bool RequestLog::trimLocked(uint64_t room)
{
    uint64_t good = 0;
    std::vector<Span> kept = scan(fd_, kHeaderBytes, bytes_, true, good);
    uint64_t total = 0;
    for (const Span &s : kept)
        total += 8 + s.length;
    size_t first = 0;
    while (first < kept.size() && total + room > kLogBytes / 4 * 3)
        total -= 8 + kept[first++].length;
    kept.erase(kept.begin(), kept.begin() + ptrdiff_t(first));
    std::vector<Span> moved;
    if (!rewriteLocked(kept, &moved))
        return false;
    if (!replayed_) {
        read_ = std::move(moved);
        added_.clear();
    }
    return true;
}

void RequestLog::add(const gm20b::Digest &name, const std::string &request)
{
    std::lock_guard<std::mutex> l(lock_);
    if (fd_ < 0 || names_.count(name) || request.size() > kRequestBytes)
        return;
    if (bytes_ + 8 + request.size() > kLogBytes && !trimLocked(8 + request.size()))
        return;
    if (!append(fd_, request))
        return;
    const Span span = {bytes_ + 8, uint32_t(request.size())};
    bytes_ += 8 + request.size();
    names_.insert(name);
    if (!replayed_)
        added_.emplace_back(name, span);
}

void RequestLog::replay(const std::function<bool(const std::string &request, gm20b::Digest &name)> &keep)
{
    std::vector<Span> read;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> l(lock_);
        read.swap(read_);
        generation = generation_;
    }
    std::vector<std::pair<gm20b::Digest, Span>> kept;
    std::set<gm20b::Digest> seen;
    std::string request;
    for (const Span &s : read) {
        gm20b::Digest name{};
        bool got;
        {
            std::lock_guard<std::mutex> l(lock_);
            if (generation != generation_)
                break;
            got = fd_ >= 0 && readSpan(fd_, s, request);
        }
        if (got && keep(request, name) && seen.insert(name).second)
            kept.emplace_back(name, s);
    }
    request = std::string();
    std::lock_guard<std::mutex> l(lock_);
    replayed_ = true;
    // a full log written again meanwhile holds what it kept, at other places than `kept`'s
    if (fd_ < 0 || generation != generation_) {
        added_.clear();
        return;
    }
    // the file already holds exactly `kept` then `added_` when the replay dropped nothing
    bool unchanged = kept.size() == read.size();
    for (const auto &entry : added_) {
        if (seen.insert(entry.first).second) {
            kept.push_back(entry);
        } else {
            unchanged = false;
        }
    }
    added_.clear();
    std::vector<Span> spans;
    for (const auto &entry : kept)
        spans.push_back(entry.second);
    if (unchanged || rewriteLocked(spans))
        names_ = std::move(seen);
}

}  // namespace switch_compiler
