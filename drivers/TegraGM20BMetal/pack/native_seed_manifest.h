// SPDX-License-Identifier: MIT
#pragma once
#import <Foundation/Foundation.h>
#include "../cache_io.h"
#include "../program_pages.h"
#include <cerrno>
#include <cstring>
#include <map>
#include <sys/stat.h>

namespace gm20b_seed {

// One line on stderr naming the step that failed, its path and errno (0: none the system gave); false
inline bool failed(const char *step, const std::string &path, int err = 0)
{
    if (err)
        fprintf(stderr, "native-seed: %s %s: %s (errno %d)\n", step, path.c_str(), strerror(err), err);
    else
        fprintf(stderr, "native-seed: %s %s\n", step, path.c_str());
    return false;
}

inline int posixCode(NSError *error)
{
    NSError *underlying = error.userInfo[NSUnderlyingErrorKey];
    return [underlying.domain isEqualToString:NSPOSIXErrorDomain] ? int(underlying.code)
           : [error.domain isEqualToString:NSPOSIXErrorDomain]    ? int(error.code)
                                                                  : 0;
}

// `path` at `mode`, chmod only where it is not: a directory another user made refuses chmod (EPERM) even when it
// already has the mode the outputs need
inline bool withMode(const std::string &path, mode_t mode)
{
    struct stat st;
    if (stat(path.c_str(), &st))
        return failed("stat", path, errno);
    if ((st.st_mode & 07777) == mode || !chmod(path.c_str(), mode))
        return true;
    return failed("chmod", path, errno);
}

inline std::string hex(uint64_t value)
{
    char text[17];
    snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return text;
}

inline bool parseHex(NSString *text, uint8_t *out, size_t bytes)
{
    if (![text isKindOfClass:[NSString class]] || text.length != bytes * 2)
        return false;
    const char *s = text.UTF8String;
    for (size_t i = 0; i < bytes; ++i) {
        const auto digit = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        const int a = digit(s[2 * i]), b = digit(s[2 * i + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = uint8_t(a * 16 + b);
    }
    return true;
}

inline bool parseStack(NSString *text, uint64_t &out)
{
    uint8_t bytes[8];
    if (!parseHex(text, bytes, sizeof bytes))
        return false;
    out = 0;
    for (uint8_t byte : bytes)
        out = (out << 8) | byte;
    return true;
}

inline NSString *airText(const uint8_t *air)
{
    char text[65];
    for (unsigned i = 0; i < 32; ++i)
        snprintf(text + 2 * i, 3, "%02x", air[i]);
    return @(text);
}

inline void identify(NSMutableDictionary *row, const gm20b::CacheKey &key)
{
    row[@"key"] = @(key.key.c_str());
    row[@"name"] = @(hex(key.name()).c_str());
    row[@"vertexAir"] = airText(key.vertexAir);
    row[@"fragmentAir"] = airText(key.fragmentAir);
}

inline bool writeManifest(const std::string &output, uint64_t stack, NSArray *rows)
{
    NSDictionary *manifest = @{
        @"schema" : @1,
        @"stack" : @(hex(stack).c_str()),
        @"packVersion" : @(gm20b::kPackVersion),
        @"cacheVersion" : @(gm20b::kEntryFormat),
        @"records" : rows
    };
    NSError *error = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:manifest options:NSJSONWritingSortedKeys error:&error];
    const auto path = output + "/manifest.json";
    if (!json)
        return failed("serialize the manifest", path);
    if (![json writeToFile:@(path.c_str()) options:NSDataWritingAtomic error:&error])
        return failed("write", path, posixCode(error));
    return withMode(path, 0644);
}

inline bool openOutput(const std::string &output, uint64_t stack, gm20b::RecordCache &cache)
{
    NSError *error = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:@(output.c_str())
                                   withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:&error])
        return failed("create the output directory", output, posixCode(error));
    if (!withMode(output, 0755))
        return false;
    errno = 0;
    if (!cache.attach(output, stack, 0))
        return failed("attach the record cache in", output, errno);
    return withMode(output + "/rec-" + hex(stack), 0755);
}

inline bool recordIdentity(const std::string &bytes, gm20b::CacheKey &key, gm20b::RuntimeRecord &record,
                           gm20b::ComputeRecord &compute, bool &kernel)
{
    gm20b::Reader reader(bytes);
    char magic[sizeof gm20b::kEntryMagic];
    reader.bytes(magic, sizeof magic);
    const auto version = reader.value<uint32_t>(), kind = reader.value<uint32_t>();
    key.stack = reader.value<uint64_t>();
    reader.bytes(key.vertexAir, sizeof key.vertexAir);
    reader.bytes(key.fragmentAir, sizeof key.fragmentAir);
    key.key = reader.text();
    std::string refusal;
    if (!reader.ok() || memcmp(magic, gm20b::kEntryMagic, sizeof magic) || version != gm20b::kEntryFormat)
        return false;
    kernel = kind == gm20b::kKindKernel;
    if (kernel)
        return gm20b::decodeKernelEntry(bytes, key, compute, refusal) == gm20b::RecordCache::Found::Record &&
               gm20b::encodeKernelEntry(key, &compute, "") == bytes;
    return kind == gm20b::kKindRecord &&
           gm20b::decodeEntry(bytes, key, record, refusal) == gm20b::RecordCache::Found::Record &&
           gm20b::encodeEntry(key, &record, "") == bytes;
}

// A render record's program slots of a page or more, each written as the entry the runtime runs in place
// (programSlotEntryBytes) to <output>/raw-<stack>/<seedSlotName>.slot and named in `slots`
inline bool writeSlots(const gm20b::RuntimeRecord &record, uint64_t stack, const std::string &output,
                       NSArray *__strong &slots)
{
    NSMutableArray *written = [NSMutableArray array];
    for (const gm20b::CapturedProgram *program : gm20b::slotPrograms(record)) {
        if (program->image.empty())
            continue;
        gm20b::PackProgram info;
        std::vector<uint8_t> slot = gm20b::programSlot(*program, info);
        if (slot.size() < gm20b::kProgramSlotPage)
            continue;
        const uint64_t name = gm20b::seedSlotName(slot.data(), slot.size());
        slot.resize(gm20b::programSlotEntryBytes(slot.size()));
        const std::string relative = "rec-" + hex(stack) + "/" + hex(name) + ".slot";
        NSData *bytes = [NSData dataWithBytesNoCopy:slot.data() length:slot.size() freeWhenDone:NO];
        if (![bytes writeToFile:@((output + "/" + relative).c_str()) atomically:YES])
            return failed("write", output + "/" + relative, errno);
        if (!withMode(output + "/" + relative, 0644))
            return false;
        [written addObject:@{@"name" : @(hex(name).c_str()), @"path" : @(relative.c_str())}];
    }
    slots = written.count ? written : nil;
    return true;
}

inline bool restampRow(NSMutableDictionary *row, const std::string &input, uint64_t oldStack, uint64_t stack,
                       const std::string &output, gm20b::RecordCache &cache, std::map<uint64_t, std::string> &written)
{
    if (![row[@"status"] isKindOfClass:[NSString class]])
        return false;
    const bool made = [row[@"status"] isEqualToString:@"made"];
    if (!row[@"key"])
        return !made && !row[@"name"] && !row[@"artifact"];
    if (![row[@"key"] isKindOfClass:[NSString class]])
        return false;
    gm20b::CacheKey key;
    key.stack = oldStack;
    key.key = [row[@"key"] UTF8String];
    if (!parseHex(row[@"vertexAir"], key.vertexAir, sizeof key.vertexAir) ||
        !parseHex(row[@"fragmentAir"], key.fragmentAir, sizeof key.fragmentAir) ||
        ![row[@"name"] isEqual:@(hex(key.name()).c_str())])
        return false;
    const auto original = "rec-" + hex(oldStack) + "/" + hex(key.name()) + ".rec";
    if (!made) {
        if (row[@"artifact"])
            return false;
        key.stack = stack;
        identify(row, key);
        return true;
    }
    if (![row[@"artifact"] isEqual:@(original.c_str())])
        return false;
    NSData *data = [NSData dataWithContentsOfFile:@((input + "/" + original).c_str())];
    if (!data || data.length > (1u << 24))
        return false;
    const std::string bytes(static_cast<const char *>(data.bytes), data.length);
    gm20b::CacheKey decoded;
    gm20b::RuntimeRecord record;
    gm20b::ComputeRecord compute;
    bool kernel = false;
    if (!recordIdentity(bytes, decoded, record, compute, kernel) || decoded.stack != key.stack ||
        decoded.key != key.key || memcmp(decoded.vertexAir, key.vertexAir, sizeof key.vertexAir) ||
        memcmp(decoded.fragmentAir, key.fragmentAir, sizeof key.fragmentAir))
        return false;
    if ((row[@"kind"] && ![row[@"kind"] isEqualToString:kernel ? @"compute" : @"render"]) ||
        (kernel && key.key.rfind("kernel | ", 0) != 0))
        return false;
    key.stack = stack;
    const auto encoded = kernel ? gm20b::encodeKernelEntry(key, &compute, "") : gm20b::encodeEntry(key, &record, "");
    const auto [at, inserted] = written.try_emplace(key.name(), encoded);
    if (!inserted && at->second != encoded)
        return false;
    const auto relative = "rec-" + hex(stack) + "/" + hex(key.name()) + ".rec";
    if (inserted) {
        gm20b::RuntimeRecord loaded;
        gm20b::ComputeRecord loadedCompute;
        std::string refusal;
        const bool stored =
            kernel ? cache.storeKernel(key, &compute, "") &&
                         cache.loadKernel(key, loadedCompute, refusal) == gm20b::RecordCache::Found::Record &&
                         gm20b::encodeKernelEntry(key, &loadedCompute, "") == encoded
                   : cache.store(key, &record, "") &&
                         cache.load(key, loaded, refusal) == gm20b::RecordCache::Found::Record &&
                         gm20b::encodeEntry(key, &loaded, "") == encoded;
        if (!stored || !withMode(output + "/" + relative, 0644))
            return false;
    }
    NSArray *slots = nil;
    if (!kernel && !writeSlots(record, stack, output, slots))
        return false;
    identify(row, key);
    row[@"artifact"] = @(relative.c_str());
    row[@"slots"] = slots;
    return true;
}

inline int restamp(int argc, char **argv)
{
    uint64_t stack;
    if (argc < 5 || !parseStack(@(argv[2]), stack)) {
        fprintf(stderr, "usage: native-seed --restamp stack output input...\n");
        return 2;
    }
    const std::string output = argv[3];
    gm20b::RecordCache cache;
    if (!openOutput(output, stack, cache))
        return 1;
    NSMutableArray *rows = [NSMutableArray new];
    std::map<uint64_t, std::string> written;
    for (int arg = 4; arg < argc; ++arg) {
        NSData *data = [NSData dataWithContentsOfFile:@((std::string(argv[arg]) + "/manifest.json").c_str())];
        NSError *error = nil;
        id manifest = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
        uint64_t oldStack;
        if (![manifest isKindOfClass:[NSDictionary class]] || ![manifest[@"schema"] isEqual:@1] ||
            ![manifest[@"packVersion"] isEqual:@(gm20b::kPackVersion)] ||
            ![manifest[@"cacheVersion"] isEqual:@(gm20b::kEntryFormat)] || !parseStack(manifest[@"stack"], oldStack) ||
            ![manifest[@"records"] isKindOfClass:[NSArray class]]) {
            failed("read the manifest of another build or none in", argv[arg], posixCode(error));
            return 1;
        }
        for (id source in manifest[@"records"]) {
            @autoreleasepool {
                if (![source isKindOfClass:[NSDictionary class]]) {
                    failed("read a record row of", std::string(argv[arg]) + "/manifest.json");
                    return 1;
                }
                NSMutableDictionary *row = [source mutableCopy];
                if (!restampRow(row, argv[arg], oldStack, stack, output, cache, written)) {
                    fprintf(stderr, "invalid or conflicting native record in %s\n", argv[arg]);
                    return 1;
                }
                [rows addObject:row];
            }
        }
    }
    return writeManifest(output, stack, rows) ? 0 : 1;
}

}  // namespace gm20b_seed
