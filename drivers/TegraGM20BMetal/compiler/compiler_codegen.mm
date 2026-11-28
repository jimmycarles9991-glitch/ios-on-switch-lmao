// SPDX-License-Identifier: MIT
// See compiler_codegen.h.
#import "compiler_codegen.h"

#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <os/log.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/stat.h>
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <sys/sysctl.h>
#endif
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include "MetalABI.h"
#include "SwitchCompileReply.h"
#include "TegraGM20BCompilerABI.h"
#include "../codegen_request.h"
#include "../library_entry.h"

namespace switch_compiler {
namespace {

using Reply = void (^)(int32_t status, const void *reply, size_t length, const char *message);
using Create = void *(*)(const char *);
using Build = void (*)(void *, void *, uint32_t, const void *, size_t, Reply);

os_log_t logger()
{
    static os_log_t l = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "compiler");
    return l;
}

// A kept reply (m2v's frontend NBCompileReplyHeader, which the seed ships and tools/gpu/shaders/replies.py reads): "NSC1",
// the warnings' and the metallib's sizes and the SHA-256 of both, then both. The device leaves the digest zero and reads
// none: a reply it reads came from its own writer (synced, renamed in) or the seed's.
struct ReplyHeader {
    uint32_t magic, warningBytes;
    uint64_t metallibBytes;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
};

static_assert(sizeof(ReplyHeader) == 48, "the reply file's header");
constexpr uint32_t kReplyMagic = 0x3143534e;

bool compilerUuid(const void *symbol, uint8_t uuid[16])
{
    Dl_info info = {};
    if (!dladdr(symbol, &info) || !info.dli_fbase)
        return false;
    const auto *header = static_cast<const mach_header_64 *>(info.dli_fbase);
    const auto *command = reinterpret_cast<const uint8_t *>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; i++, command += reinterpret_cast<const load_command *>(command)->cmdsize)
        if (reinterpret_cast<const load_command *>(command)->cmd == LC_UUID) {
            memcpy(uuid, reinterpret_cast<const uuid_command *>(command)->uuid, 16);
            return std::any_of(uuid, uuid + 16, [](uint8_t b) {
                return b != 0;
            });
        }
    return false;
}

std::string replyName(const uint8_t uuid[16], uint32_t type, const std::string &request)
{
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, uuid, 16);
    CC_SHA256_Update(&c, &type, sizeof type);
    CC_SHA256_Update(&c, request.data(), CC_LONG(request.size()));
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &c);
    return switch_compile_reply::hex(digest, sizeof digest) + ".reply";
}

bool readReply(const std::string &dir, const std::string &name, gm20b::MadeLibrary &out)
{
    if (dir.empty())
        return false;
    NSData *file = [NSData dataWithContentsOfFile:@((dir + "/" + name).c_str())
                                          options:NSDataReadingMappedIfSafe
                                            error:nil];
    ReplyHeader h;
    if (file.length < sizeof h)
        return false;
    memcpy(&h, file.bytes, sizeof h);
    if (h.magic != kReplyMagic || !h.metallibBytes ||
        sizeof h + uint64_t(h.warningBytes) + h.metallibBytes != file.length)
        return false;
    const auto *payload = static_cast<const char *>(file.bytes) + sizeof h;
    out.warnings.assign(payload, h.warningBytes);
    out.metallib.assign(payload + h.warningBytes, size_t(h.metallibBytes));
    return true;
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
std::string osBuild()
{
    char value[128] = {};
    size_t bytes = sizeof value;
    return sysctlbyname("kern.osversion", value, &bytes, nullptr, 0) == 0 && bytes && bytes <= sizeof value &&
                   value[bytes - 1] == 0
               ? std::string(value)
               : std::string();
}
#endif

bool writeSynced(const std::string &path, NSData *bytes)
{
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        return false;
    const auto *at = static_cast<const uint8_t *>(bytes.bytes);
    size_t left = bytes.length;
    while (left) {
        const ssize_t wrote = write(fd, at, left);
        if (wrote <= 0)
            break;
        at += wrote;
        left -= size_t(wrote);
    }
    const bool written = left == 0 && fsync(fd) == 0;
    close(fd);
    return written;
}

// Written beside its name, synced and renamed in, then its request and provenance (SwitchCompileReply.h) as the harvest
// reads them
void keepReply(const std::string &dir, const std::string &name, const uint8_t uuid[16], uint32_t type,
               const std::string &request, const gm20b::MadeLibrary &made)
{
    ReplyHeader h = {kReplyMagic, uint32_t(made.warnings.size()), made.metallib.size(), {}};
    NSMutableData *file = [NSMutableData dataWithCapacity:sizeof h + made.warnings.size() + made.metallib.size()];
    [file appendBytes:&h length:sizeof h];
    [file appendBytes:made.warnings.data() length:made.warnings.size()];
    [file appendBytes:made.metallib.data() length:made.metallib.size()];
    static std::atomic<uint64_t> tempSeq{0};
    const std::string path = dir + "/" + name,
                      temporary = path + "." + std::to_string(getpid()) + "." +
                                  std::to_string(tempSeq.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    if (!writeSynced(temporary, file) || rename(temporary.c_str(), path.c_str())) {
        unlink(temporary.c_str());
        return;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    NSData *metallib = [NSData dataWithBytesNoCopy:const_cast<char *>(made.metallib.data())
                                            length:made.metallib.size()
                                      freeWhenDone:NO];
    static const std::string build = osBuild();
    switch_compile_reply::write(dir, name, uuid, type, request.data(), request.size(), file, metallib, build,
                                SWITCH_METAL_KIND);
#else
    (void)uuid;
    (void)type;
    (void)request;
#endif
}

void *runWork(void *work)
{
    @autoreleasepool {
        (*static_cast<const std::function<void()> *>(work))();
    }
    return nullptr;
}

// The front end recurses deeply (it was written for MTLCompilerService's threads): a pool worker's 512 KiB stack is too
// small for it, so it runs on a thread of its own with a 16 MiB stack, at the class of the worker that waits for it
bool onLargeStack(const std::function<void()> &work)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr))
        return false;
    const qos_class_t qos = qos_class_self();
    pthread_t thread;
    const bool made = !pthread_attr_setstacksize(&attr, 16u << 20) &&
                      (qos == QOS_CLASS_UNSPECIFIED || !pthread_attr_set_qos_class_np(&attr, qos, 0)) &&
                      !pthread_create(&thread, &attr, runWork, const_cast<std::function<void()> *>(&work));
    pthread_attr_destroy(&attr);
    if (made)
        pthread_join(thread, nullptr);
    return made;
}

struct Answer {
    bool called = false, refused = false;
    std::string log;
    gm20b::MadeLibrary made;
};

// The reply, to a block called before the request returns: a header whose u32s at 8 and 12 are its own size and that of
// the warnings after it, then the metallib
void answered(Answer &a, int32_t status, const uint8_t *reply, size_t length, const char *message)
{
    a.called = true;
    uint32_t header = 0, warnings = 0;
    if (reply && length >= 16) {
        memcpy(&header, reply + 8, 4);
        memcpy(&warnings, reply + 12, 4);
    }
    if (status || !reply || length < 16 || uint64_t(header) + warnings >= length) {
        a.refused = true;
        a.log = message && *message ? message : "the Metal compiler failed (status " + std::to_string(status) + ")";
        return;
    }
    a.made.warnings.assign(reinterpret_cast<const char *>(reply) + header, warnings);
    a.made.metallib.assign(reinterpret_cast<const char *>(reply) + header + warnings, length - header - warnings);
}

}  // namespace

struct CodeGen::State {
    CodeGenPaths paths;
    std::mutex loadLock;
    std::mutex lock;
    std::atomic<bool> loaded{false};
    std::string refusal;
    Create create = nullptr;
    Build build = nullptr;
    void *service = nullptr;
    uint8_t uuid[16] = {};

    bool load(std::string &why);
};

bool CodeGen::State::load(std::string &why)
{
    if (loaded.load(std::memory_order_acquire))
        return build ? true : (why = refusal, false);
    std::lock_guard<std::mutex> guard(loadLock);
    if (loaded.load(std::memory_order_relaxed))
        return build ? true : (why = refusal, false);
#if SWITCH_DEBUG
    const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    void *library = dlopen(SWITCH_METAL_COMPILER, RTLD_NOW | RTLD_LOCAL);
    create = library ? reinterpret_cast<Create>(dlsym(library, "MTLCodeGenServiceCreate")) : nullptr;
    Build found = library ? reinterpret_cast<Build>(dlsym(library, "MTLCodeGenServiceBuildRequest")) : nullptr;
    if (!create || !found || !compilerUuid(reinterpret_cast<const void *>(found), uuid)) {
        refusal = std::string("the system's Metal compiler did not load from " SWITCH_METAL_COMPILER ": ") +
                  (library ? "no code generation service" : dlerror());
        os_log_error(logger(), "SwitchShaderCompiler: %{public}s", refusal.c_str());
        loaded.store(true, std::memory_order_release);
        return why = refusal, false;
    }
    build = found;
    for (const std::string *dir : {&paths.modules, &paths.replies})
        mkdir(dir->c_str(), 0700);
#if SWITCH_DEBUG
    os_log(logger(), "SwitchShaderCompiler: the system's Metal compiler loaded for its first library in %llu ms",
           (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000));
#endif
    loaded.store(true, std::memory_order_release);
    return true;
}

CodeGen::CodeGen(CodeGenPaths paths) : state_(std::make_unique<State>())
{
    state_->paths = std::move(paths);
}

CodeGen::~CodeGen() = default;

int32_t CodeGen::make(const Prepared &p, std::string &entry, std::string &why)
{
    State &s = *state_;
    const gm20b::CompileRequest &r = p.request;
    const std::string request = r.codeGenType == gm20b::kCodeGenSource
                                    ? gm20b::codegen::withModuleCache(r.codeGen, s.paths.modules)
                                    : r.codeGen;
    if (request.empty())
        return why = "a library request the front end cannot read", TegraGM20BCompileUnavailable;
    if (!s.load(why))
        return TegraGM20BCompileUnavailable;
    const std::string name = replyName(s.uuid, r.codeGenType, request);
    gm20b::MadeLibrary kept;
    if (readReply(s.paths.replies, name, kept) || readReply(s.paths.seed, name, kept)) {
        entry = gm20b::encodeLibraryEntry(p.content, &kept, "");
        return TegraGM20BCompileMade;
    }
    Answer a;
    bool ran = false;
    {
        // Only the front end's code generation service itself is serialized; reply reads/writes stay lock-free.
        std::lock_guard<std::mutex> one(s.lock);
        if (!s.service && !(s.service = s.create("SwitchShaderCompiler")))
            return why = "the system's Metal compiler made no code generation service", TegraGM20BCompileUnavailable;
        Answer *answer = &a;
        ran = onLargeStack([&] {
            s.build(s.service, nullptr, r.codeGenType, request.data(), request.size(),
                    ^(int32_t status, const void *reply, size_t length, const char *message) {
                        answered(*answer, status, static_cast<const uint8_t *>(reply), length, message);
                    });
        });
    }
    if (!ran)
        return why = "no thread for the system's Metal compiler", TegraGM20BCompileFailed;
    if (!a.called)
        return why = "the system's Metal compiler did not answer", TegraGM20BCompileFailed;
    if (a.refused) {
        why = a.log;
        entry = gm20b::encodeLibraryEntry(p.content, nullptr, a.log);
        return TegraGM20BCompileRefused;
    }
    keepReply(s.paths.replies, name, s.uuid, r.codeGenType, request, a.made);
    entry = gm20b::encodeLibraryEntry(p.content, &a.made, "");
    return TegraGM20BCompileMade;
}

}  // namespace switch_compiler
