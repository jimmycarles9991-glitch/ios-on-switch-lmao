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

// Where each whole request of the file is, and the end of the last: a request that does not check ends it
std::vector<RequestLog::Span> scan(int fd, uint64_t &good)
{
    std::vector<RequestLog::Span> out;
    good = 0;
    struct stat info = {};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode))
        return out;
    const uint64_t size = std::min<uint64_t>(uint64_t(info.st_size), RequestLog::kLogBytes);
    std::string request;
    for (uint64_t at = 0; at + 8 <= size;) {
        uint32_t head[2];
        if (pread(fd, head, sizeof head, off_t(at)) != ssize_t(sizeof head) || head[0] > RequestLog::kRequestBytes ||
            head[0] > size - at - 8)
            break;
        const RequestLog::Span span = {at + 8, head[0]};
        if (!readSpan(fd, span, request) ||
            switch_seed_pack::fnv32(reinterpret_cast<const uint8_t *>(request.data()), request.size()) != head[1])
            break;
        out.push_back(span);
        at += 8 + head[0];
        good = at;
    }
    return out;
}

}  // namespace

bool RequestLog::append(int fd, const std::string &request)
{
    const uint32_t head[2] = {
        uint32_t(request.size()),
        switch_seed_pack::fnv32(reinterpret_cast<const uint8_t *>(request.data()), request.size())};
    return writeAll(fd, head, sizeof head) && writeAll(fd, request.data(), request.size());
}

bool RequestLog::open(const std::string &path)
{
    std::lock_guard<std::mutex> l(lock_);
    path_ = path;
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd_ < 0)
        return false;
    read_ = scan(fd_, bytes_);
    if (ftruncate(fd_, off_t(bytes_)) || lseek(fd_, 0, SEEK_END) < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

// lock held: `spans` of the open file (in its order) written as the log, each read on its own; the file it replaces
// closed. False: the log is as it was
bool RequestLog::rewriteLocked(const std::vector<Span> &spans)
{
    const std::string fresh = path_ + ".new";
    const int fd = ::open(fresh.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    bool written = fd >= 0;
    uint64_t bytes = 0;
    std::string request;
    for (const Span &s : spans)
        if (written && (written = readSpan(fd_, s, request) && append(fd, request)))
            bytes += 8 + s.length;
    if (!written || rename(fresh.c_str(), path_.c_str())) {
        if (fd >= 0)
            close(fd);
        unlink(fresh.c_str());
        return false;
    }
    close(fd_);
    fd_ = fd;
    bytes_ = bytes;
    generation_++;
    return true;
}

// lock held: the log written again without its oldest requests, so `room` more bytes fit in three quarters of it. Before
// the replay, what it replays is then what the log kept, added since or not
bool RequestLog::trimLocked(uint64_t room)
{
    uint64_t good = 0;
    std::vector<Span> kept = scan(fd_, good);
    uint64_t total = 0;
    for (const Span &s : kept)
        total += 8 + s.length;
    size_t first = 0;
    while (first < kept.size() && total + room > kLogBytes / 4 * 3)
        total -= 8 + kept[first++].length;
    kept.erase(kept.begin(), kept.begin() + ptrdiff_t(first));
    if (!rewriteLocked(kept))
        return false;
    if (!replayed_) {
        read_ = scan(fd_, good);
        added_.clear();
    }
    return true;
}

void RequestLog::add(const raw::Digest &name, const std::string &request)
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

void RequestLog::replay(const std::function<bool(const std::string &request, raw::Digest &name)> &keep)
{
    std::vector<Span> read;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> l(lock_);
        read.swap(read_);
        generation = generation_;
    }
    std::vector<std::pair<raw::Digest, Span>> kept;
    std::set<raw::Digest> seen;
    std::string request;
    for (const Span &s : read) {
        raw::Digest name{};
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
    for (const auto &entry : added_)
        if (seen.insert(entry.first).second)
            kept.push_back(entry);
    added_.clear();
    std::vector<Span> spans;
    for (const auto &entry : kept)
        spans.push_back(entry.second);
    if (rewriteLocked(spans))
        names_ = std::move(seen);
}

}  // namespace switch_compiler
