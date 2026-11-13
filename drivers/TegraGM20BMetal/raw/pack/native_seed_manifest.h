// SPDX-License-Identifier: MIT
#pragma once
#import <Foundation/Foundation.h>
#include "../runtime/raw_cache_io.h"
#include "../runtime/raw_program_pages.h"
#include "NativeMetalArtifactCache.h"
#include <map>
#include <sys/stat.h>

namespace rawseed {

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

inline void identify(NSMutableDictionary *row, const raw::CacheKey &key)
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
        @"packVersion" : @(raw::kPackVersion),
        @"cacheVersion" : @(raw::kEntryFormat),
        @"records" : rows
    };
    NSError *error = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:manifest options:NSJSONWritingSortedKeys error:&error];
    const auto path = output + "/manifest.json";
    return json && [json writeToFile:@(path.c_str()) options:NSDataWritingAtomic error:&error] &&
           chmod(path.c_str(), 0644) == 0;
}

inline bool openOutput(const std::string &output, uint64_t stack, raw::RecordCache &cache)
{
    NSError *error = nil;
    return [[NSFileManager defaultManager] createDirectoryAtPath:@(output.c_str())
                                     withIntermediateDirectories:YES
                                                      attributes:nil
                                                           error:&error] &&
           chmod(output.c_str(), 0755) == 0 && cache.attach(output, stack, 0) &&
           chmod((output + "/raw-" + hex(stack)).c_str(), 0755) == 0;
}

inline bool recordIdentity(const std::string &bytes, raw::CacheKey &key, raw::RuntimeRecord &record,
                           raw::ComputeRecord &compute, bool &kernel)
{
    raw::Reader reader(bytes);
    char magic[sizeof raw::kEntryMagic];
    reader.bytes(magic, sizeof magic);
    const auto version = reader.value<uint32_t>(), kind = reader.value<uint32_t>();
    key.stack = reader.value<uint64_t>();
    reader.bytes(key.vertexAir, sizeof key.vertexAir);
    reader.bytes(key.fragmentAir, sizeof key.fragmentAir);
    key.key = reader.text();
    std::string refusal;
    if (!reader.ok() || memcmp(magic, raw::kEntryMagic, sizeof magic) || version != raw::kEntryFormat)
        return false;
    kernel = kind == raw::kKindKernel;
    if (kernel)
        return raw::decodeKernelEntry(bytes, key, compute, refusal) == raw::RecordCache::Found::Record &&
               raw::encodeKernelEntry(key, &compute, "") == bytes;
    return kind == raw::kKindRecord &&
           raw::decodeEntry(bytes, key, record, refusal) == raw::RecordCache::Found::Record &&
           raw::encodeEntry(key, &record, "") == bytes;
}

// A render record's program slots of a page or more, each written as the entry the runtime runs in place
// (programSlotEntryBytes) to <output>/raw-<stack>/<seedSlotName>.slot and named in `slots`
inline bool writeSlots(const raw::RuntimeRecord &record, uint64_t stack, const std::string &output,
                       NSArray *__strong &slots)
{
    NSMutableArray *written = [NSMutableArray array];
    for (const raw::CapturedProgram *program : raw::slotPrograms(record)) {
        if (program->image.empty())
            continue;
        raw::PackProgram info;
        std::vector<uint8_t> slot = raw::programSlot(*program, info);
        if (slot.size() < raw::kProgramSlotPage)
            continue;
        const uint64_t name = raw::seedSlotName(slot.data(), slot.size());
        slot.resize(raw::programSlotEntryBytes(slot.size()));
        const std::string relative = "raw-" + hex(stack) + "/" + hex(name) + ".slot";
        NSData *bytes = [NSData dataWithBytesNoCopy:slot.data() length:slot.size() freeWhenDone:NO];
        if (![bytes writeToFile:@((output + "/" + relative).c_str()) atomically:YES] ||
            chmod((output + "/" + relative).c_str(), 0644))
            return false;
        [written addObject:@{@"name" : @(hex(name).c_str()), @"path" : @(relative.c_str())}];
    }
    slots = written.count ? written : nil;
    return true;
}

inline bool restampRow(NSMutableDictionary *row, const std::string &input, uint64_t oldStack, uint64_t stack,
                       const std::string &output, raw::RecordCache &cache, std::map<uint64_t, std::string> &written)
{
    if (![row[@"status"] isKindOfClass:[NSString class]])
        return false;
    const bool made = [row[@"status"] isEqualToString:@"made"];
    if (!row[@"key"])
        return !made && !row[@"name"] && !row[@"artifact"];
    if (![row[@"key"] isKindOfClass:[NSString class]])
        return false;
    raw::CacheKey key;
    key.stack = oldStack;
    key.key = [row[@"key"] UTF8String];
    if (!parseHex(row[@"vertexAir"], key.vertexAir, sizeof key.vertexAir) ||
        !parseHex(row[@"fragmentAir"], key.fragmentAir, sizeof key.fragmentAir) ||
        ![row[@"name"] isEqual:@(hex(key.name()).c_str())])
        return false;
    const auto original = "raw-" + hex(oldStack) + "/" + hex(key.name()) + ".rec";
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
    raw::CacheKey decoded;
    raw::RuntimeRecord record;
    raw::ComputeRecord compute;
    bool kernel = false;
    if (!recordIdentity(bytes, decoded, record, compute, kernel) || decoded.stack != key.stack || decoded.key != key.key ||
        memcmp(decoded.vertexAir, key.vertexAir, sizeof key.vertexAir) ||
        memcmp(decoded.fragmentAir, key.fragmentAir, sizeof key.fragmentAir))
        return false;
    if ((row[@"kind"] && ![row[@"kind"] isEqualToString:kernel ? @"compute" : @"render"]) ||
        (kernel && key.key.rfind("kernel | ", 0) != 0))
        return false;
    key.stack = stack;
    const auto encoded = kernel ? raw::encodeKernelEntry(key, &compute, "") : raw::encodeEntry(key, &record, "");
    const auto [at, inserted] = written.try_emplace(key.name(), encoded);
    if (!inserted && at->second != encoded)
        return false;
    const auto relative = "raw-" + hex(stack) + "/" + hex(key.name()) + ".rec";
    if (inserted) {
        raw::RuntimeRecord loaded;
        raw::ComputeRecord loadedCompute;
        std::string refusal;
        const bool stored = kernel ?
            cache.storeKernel(key, &compute, "") &&
                cache.loadKernel(key, loadedCompute, refusal) == raw::RecordCache::Found::Record &&
                raw::encodeKernelEntry(key, &loadedCompute, "") == encoded :
            cache.store(key, &record, "") && cache.load(key, loaded, refusal) == raw::RecordCache::Found::Record &&
                raw::encodeEntry(key, &loaded, "") == encoded;
        if (!stored || chmod((output + "/" + relative).c_str(), 0644))
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

inline bool copyAnalysis(NSDictionary *row, const std::string &input, const std::string &output,
                          std::map<std::string, native_artifact_cache::Key> &written)
{
    if (![row[@"status"] isKindOfClass:[NSString class]])
        return false;
    if (![row[@"status"] isEqualToString:@"made"])
        return !row[@"analysis"];
    NSArray *artifacts = row[@"analysis"];
    if (![artifacts isKindOfClass:[NSArray class]] || artifacts.count != 2)
        return false;
    unsigned contracts = 0;
    for (id entry in artifacts) {
        if (![entry isKindOfClass:[NSDictionary class]] || ![entry[@"name"] isKindOfClass:[NSString class]] ||
            !([entry[@"reflection"] isEqual:@NO] || [entry[@"reflection"] isEqual:@YES]))
            return false;
        const unsigned contract = [entry[@"reflection"] boolValue] ? 2 : 1;
        if (contracts & contract)
            return false;
        contracts |= contract;
        NSString *name = entry[@"name"];
        native_artifact_cache::Key key = {}, digest = {};
        if (name.length != 68 || ![name hasSuffix:@".nca"] ||
            !parseHex([name substringToIndex:64], key.data(), key.size()))
            return false;
        const std::string filename = name.UTF8String;
        const std::string relative = "native-analysis/" + filename;
        if (![entry[@"path"] isEqual:@(relative.c_str())])
            return false;
        native_artifact_cache::Mapping mapped;
        if (!mapped.load(input + "/native-analysis", key))
            return false;
        CC_SHA256(mapped.data(), CC_LONG(mapped.size()), digest.data());
        const auto [at, inserted] = written.emplace(filename, digest);
        if (!inserted && at->second != digest)
            return false;
        if (inserted && (!native_artifact_cache::store(output + "/native-analysis", key, mapped.data(), mapped.size()) ||
                         chmod((output + "/" + relative).c_str(), 0644) ||
                         chmod((output + "/native-analysis").c_str(), 0755)))
            return false;
    }
    return contracts == 3;
}

inline int restamp(int argc, char **argv)
{
    uint64_t stack;
    if (argc < 5 || !parseStack(@(argv[2]), stack))
        return 2;
    const std::string output = argv[3];
    raw::RecordCache cache;
    if (!openOutput(output, stack, cache))
        return 1;
    NSMutableArray *rows = [NSMutableArray new];
    std::map<uint64_t, std::string> written;
    std::map<std::string, native_artifact_cache::Key> analysisWritten;
    for (int arg = 4; arg < argc; ++arg) {
        NSData *data = [NSData dataWithContentsOfFile:@((std::string(argv[arg]) + "/manifest.json").c_str())];
        NSError *error = nil;
        id manifest = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
        uint64_t oldStack;
        if (![manifest isKindOfClass:[NSDictionary class]] || ![manifest[@"schema"] isEqual:@1] ||
            ![manifest[@"packVersion"] isEqual:@(raw::kPackVersion)] ||
            ![manifest[@"cacheVersion"] isEqual:@(raw::kEntryFormat)] || !parseStack(manifest[@"stack"], oldStack) ||
            ![manifest[@"records"] isKindOfClass:[NSArray class]])
            return 1;
        for (id source in manifest[@"records"]) {
            @autoreleasepool {
                if (![source isKindOfClass:[NSDictionary class]])
                    return 1;
                NSMutableDictionary *row = [source mutableCopy];
                const bool valid = [row[@"kind"] isEqual:@"analysis"]
                    ? copyAnalysis(row, argv[arg], output, analysisWritten)
                    : restampRow(row, argv[arg], oldStack, stack, output, cache, written);
                if (!valid) {
                    fprintf(stderr, "invalid or conflicting native record in %s\n", argv[arg]);
                    return 1;
                }
                [rows addObject:row];
            }
        }
    }
    return writeManifest(output, stack, rows) ? 0 : 1;
}

}  // namespace rawseed
