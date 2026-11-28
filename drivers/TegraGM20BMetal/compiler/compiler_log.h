// SPDX-License-Identifier: MIT
// The requests the compiler made a record for, kept across builds in <root>/requests: when a new build or pack starts a
// new store, the compiler makes each of them again at the background class before a client asks (compiler_service.mm), so
// the first launch after an update waits on no compile the device made before. A request is kept once (by its store
// name): a header (its magic, the durable end and the boot of its last writer, as the store's in SwitchSeedStore.h),
// then each request's length, a mark and its bytes, appended. Nothing reads a request's bytes to decide it is whole:
// within a boot the page cache holds every append whole, and a reader in a later boot reads no further than the durable
// end, which persist() moves only past what it synced. The file is written again at a replay that drops a request (only
// those that still prepare), and without its oldest when a request would not fit; each new file is synced before it is
// renamed in.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "../compile_request.h"

namespace switch_compiler {

class RequestLog {
public:
    static constexpr uint64_t kRequestBytes = uint64_t(1) << 20, kLogBytes = uint64_t(32) << 20;
    static constexpr uint64_t kHeaderBytes = 64;

    // A request's bytes in the file: the log keeps where its requests are, never their bytes
    struct Span {
        uint64_t at;
        uint32_t length;
    };

    // Its requests found; false: none kept (a request is then only compiled). `boot` names the boot the writer runs in
    // (null: this one), as SwitchSeedStore.h's StoreWriter::open takes it.
    bool open(const std::string &path, const char *boot = nullptr);
    // `request` kept under `name` unless it is kept already or too large; a full log drops its oldest first
    void add(const gm20b::Digest &name, const std::string &request);
    // The requests read at open, then the log written again with only those `keep` returns true for (and every one added
    // since): what a replay calls once
    void replay(const std::function<bool(const std::string &request, gm20b::Digest &name)> &keep);
    // Every request appended so far synced, then the durable end moved past them. Appends wait for none of it.
    bool persist();

private:
    std::mutex lock_;
    std::string path_;
    int fd_ = -1;
    uint64_t bytes_ = 0, durable_ = 0, generation_ = 0;  // generation_: the file's rewrites, which move its spans
    char boot_[48] = {};
    bool replayed_ = false;
    std::set<gm20b::Digest> names_;
    std::vector<Span> read_;
    std::vector<std::pair<gm20b::Digest, Span>> added_;  // before the replay, which keeps them

    bool rewriteLocked(const std::vector<Span> &spans, std::vector<Span> *moved = nullptr);
    bool trimLocked(uint64_t room);
};

}  // namespace switch_compiler
