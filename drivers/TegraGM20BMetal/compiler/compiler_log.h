// SPDX-License-Identifier: MIT
// The requests the compiler made a record for, kept across builds in <root>/requests: when a new build or pack starts a
// new store, the compiler makes each of them again at the background class before a client asks (compiler_service.mm), so
// the first launch after an update waits on no compile the device made before. A request is kept once (by its store
// name), whole or not at all: each is its length, its checksum and its bytes, appended, and a reader stops at the first
// that does not check. The file is written again at each replay with only the requests that still prepare, and without its
// oldest when a request would not fit.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "../raw/runtime/raw_compile_request.h"

namespace switch_compiler {

class RequestLog {
public:
    static constexpr uint64_t kRequestBytes = uint64_t(1) << 20, kLogBytes = uint64_t(32) << 20;

    // A request's bytes in the file: the log keeps where its requests are, never their bytes
    struct Span {
        uint64_t at;
        uint32_t length;
    };

    // Its requests found; false: none kept (a request is then only compiled)
    bool open(const std::string &path);
    // `request` kept under `name` unless it is kept already or too large; a full log drops its oldest first
    void add(const raw::Digest &name, const std::string &request);
    // The requests read at open, then the log written again with only those `keep` returns true for (and every one added
    // since): what a replay calls once
    void replay(const std::function<bool(const std::string &request, raw::Digest &name)> &keep);

private:
    std::mutex lock_;
    std::string path_;
    int fd_ = -1;
    uint64_t bytes_ = 0, generation_ = 0;  // generation_: the file's rewrites, which move its spans
    bool replayed_ = false;
    std::set<raw::Digest> names_;
    std::vector<Span> read_;
    std::vector<std::pair<raw::Digest, Span>> added_;  // before the replay, which keeps them

    bool append(int fd, const std::string &request);
    bool rewriteLocked(const std::vector<Span> &spans);
    bool trimLocked(uint64_t room);
};

}  // namespace switch_compiler
