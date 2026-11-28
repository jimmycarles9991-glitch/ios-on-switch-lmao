// SPDX-License-Identifier: MIT
#pragma once
#import <Foundation/Foundation.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unistd.h>

namespace switch_compile_reply {

constexpr size_t kRequestLimit = 64u << 20;

inline std::string hex(const void *bytes, size_t length)
{
    const auto *input = static_cast<const uint8_t *>(bytes);
    constexpr char digits[] = "0123456789abcdef";
    std::string out(length * 2, '\0');
    for (size_t i = 0; i < length; ++i) {
        out[2 * i] = digits[input[i] >> 4];
        out[2 * i + 1] = digits[input[i] & 15];
    }
    return out;
}

inline bool component(const std::string &value)
{
    return !value.empty() && value.size() <= 64 &&
           value.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.-_") ==
               std::string::npos &&
           value != "." && value != "..";
}

inline bool first(const std::string &directory, const std::string &name)
{
    static std::mutex lock;
    static std::unordered_set<std::string> recorded;
    std::lock_guard<std::mutex> guard(lock);
    if (recorded.size() >= 4096)
        recorded.clear();
    return recorded.insert(directory + "/" + name).second;
}

// The reply's header names a metallib of `metallib`'s length after its warnings
inline bool matches(NSData *reply, NSData *metallib)
{
    if (reply.length < 48 || reply.length > UINT32_MAX || !metallib.length)
        return false;
    const auto *bytes = static_cast<const uint8_t *>(reply.bytes);
    uint32_t warnings = 0;
    uint64_t length = 0;
    memcpy(&warnings, bytes + 4, sizeof warnings);
    memcpy(&length, bytes + 8, sizeof length);
    return !memcmp(bytes, "NSC1", 4) && warnings <= reply.length - 48 && length == reply.length - 48 - warnings &&
           length == metallib.length;
}

// The reply `name` (replyName's, which tools/gpu/shaders/replies.py makes again from the request) gets its request and
// provenance beside it. The provenance carries no digests: the harvest hashes the files on the host.
inline bool write(const std::string &directory, const std::string &name, const uint8_t uuid[16], uint32_t type,
                  const void *request, size_t requestBytes, NSData *reply, NSData *metallib, const std::string &osBuild,
                  const std::string &targetBuild)
{
    if (!component(osBuild) || !component(targetBuild) || requestBytes > kRequestLimit || (!request && requestBytes) ||
        !uuid || !matches(reply, metallib) || name.size() != 64 + 6 || std::all_of(uuid, uuid + 16, [](uint8_t value) {
            return value == 0;
        }))
        return false;
    const std::string stem = name.substr(0, 64), relative = "provenance/" + osBuild + "/" + stem + ".json";
    NSDictionary *record = @{
        @"schema" : @2,
        @"reply" : @(name.c_str()),
        @"compilerUUID" : @(hex(uuid, 16).c_str()),
        @"requestType" : @(type),
        @"requestBytes" : @(requestBytes),
        @"osBuild" : @(osBuild.c_str()),
        @"targetBuild" : @(targetBuild.c_str())
    };
    NSError *error = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingSortedKeys error:&error];
    NSData *input = [NSData dataWithBytesNoCopy:const_cast<void *>(request) length:requestBytes freeWhenDone:NO];
    NSString *folder = @((directory + "/provenance/" + osBuild).c_str());
    if (!json ||
        ![[NSFileManager defaultManager] createDirectoryAtPath:folder
                                   withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:&error] ||
        ![input writeToFile:@((directory + "/" + stem + ".request").c_str()) options:NSDataWritingAtomic error:&error])
        return false;
    return [json writeToFile:@((directory + "/" + relative).c_str()) options:NSDataWritingAtomic error:&error];
}

}  // namespace switch_compile_reply
