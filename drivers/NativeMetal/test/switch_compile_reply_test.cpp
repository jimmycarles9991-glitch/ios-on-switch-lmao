// SPDX-License-Identifier: MIT
#include "drivers/NativeMetal/SwitchCompileReply.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {
using namespace switch_compile_reply;

class Reply : public testing::Test {
protected:
    std::string directory;
    std::array<uint8_t, 16> uuid{};
    std::string request = "source with ";
    std::string name;
    NSData *output = nil;
    NSMutableData *reply = nil;

    void SetUp() override
    {
        directory =
            std::string(NSTemporaryDirectory().UTF8String) + "switch-reply-" + NSUUID.UUID.UUIDString.UTF8String;
        ASSERT_TRUE([[NSFileManager defaultManager] createDirectoryAtPath:@(directory.c_str())
                                              withIntermediateDirectories:YES
                                                               attributes:nil
                                                                    error:nil]);
        request.append("\0tail", 5);
        for (size_t i = 0; i < uuid.size(); ++i)
            uuid[i] = uint8_t(i);
        const uint32_t type = 13;
        std::string key(reinterpret_cast<const char *>(uuid.data()), uuid.size());
        key.append(reinterpret_cast<const char *>(&type), sizeof type);
        key.append(request);
        name = digest(key.data(), key.size()) + ".reply";
        output = [@"MTLB retained output" dataUsingEncoding:NSUTF8StringEncoding];
        reply = [NSMutableData dataWithLength:48];
        auto *header = static_cast<uint8_t *>(reply.mutableBytes);
        memcpy(header, "NSC1", 4);
        const uint32_t warnings = 3;
        const uint64_t bytes = output.length;
        memcpy(header + 4, &warnings, sizeof warnings);
        memcpy(header + 8, &bytes, sizeof bytes);
        [reply appendBytes:"abc" length:3];
        [reply appendData:output];
        header = static_cast<uint8_t *>(reply.mutableBytes);
        CC_SHA256(header + 48, CC_LONG(reply.length - 48), header + 16);
        ASSERT_TRUE([reply writeToFile:@((directory + "/" + name).c_str()) atomically:YES]);
    }

    void TearDown() override
    {
        [[NSFileManager defaultManager] removeItemAtPath:@(directory.c_str()) error:nil];
    }

    bool publish(const std::string &os = "24B119", const std::string &target = "24B119")
    {
        return write(directory, name, uuid.data(), 13, request.data(), request.size(), reply, output, os, target);
    }

    NSDictionary *record(const std::string &os = "24B119")
    {
        const auto path = directory + "/provenance/" + os + "/" + name.substr(0, 64) + ".json";
        NSData *data = [NSData dataWithContentsOfFile:@(path.c_str())];
        return data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    }
};

TEST_F(Reply, ExactSourceCompilerAndOutputCommit)
{
    ASSERT_TRUE(publish());
    NSDictionary *metadata = record();
    ASSERT_NE(metadata, nil);
    EXPECT_TRUE([metadata[@"compilerUUID"] isEqualToString:@(hex(uuid.data(), uuid.size()).c_str())]);
    EXPECT_TRUE([metadata[@"requestSHA256"] isEqualToString:@(digest(request.data(), request.size()).c_str())]);
    EXPECT_TRUE([metadata[@"metallibSHA256"] isEqualToString:@(digest(output.bytes, output.length).c_str())]);
    EXPECT_EQ([metadata[@"requestType"] unsignedIntValue], 13u);
    const auto path = directory + "/" + name.substr(0, 64) + ".request";
    NSData *source = [NSData dataWithContentsOfFile:@(path.c_str())];
    ASSERT_EQ(source.length, request.size());
    EXPECT_EQ(memcmp(source.bytes, request.data(), request.size()), 0);
}

TEST_F(Reply, ActualOsRemainsDistinctFromTarget)
{
    ASSERT_TRUE(publish("25A123", "24B119"));
    EXPECT_TRUE([record("25A123")[@"osBuild"] isEqualToString:@"25A123"]);
    EXPECT_TRUE([record("25A123")[@"targetBuild"] isEqualToString:@"24B119"]);
    EXPECT_EQ(record(), nil);
    ASSERT_TRUE(publish("21H461", "21H461"));
    EXPECT_NE(record("25A123"), nil);
    EXPECT_NE(record("21H461"), nil);
}

TEST_F(Reply, InvalidKeyNeverPublishes)
{
    name[0] = name[0] == '0' ? '1' : '0';
    EXPECT_FALSE(publish());
    EXPECT_EQ(record(), nil);
}

TEST_F(Reply, RequestBoundCheckedBeforeRead)
{
    EXPECT_FALSE(write(directory, name, uuid.data(), 13, reinterpret_cast<void *>(1), kRequestLimit + 1, reply, output,
                       "24B119", "24B119"));
    EXPECT_EQ(record(), nil);
}

TEST_F(Reply, ReplyEnvelopeAndOutputMustMatch)
{
    NSData *wrong = [@"different output" dataUsingEncoding:NSUTF8StringEncoding];
    EXPECT_FALSE(
        write(directory, name, uuid.data(), 13, request.data(), request.size(), reply, wrong, "24B119", "24B119"));
    auto *bytes = static_cast<uint8_t *>(reply.mutableBytes);
    bytes[16] ^= 1;
    EXPECT_FALSE(publish());
    EXPECT_EQ(record(), nil);
}

TEST_F(Reply, ChangedInstalledReplyCannotCommitMetadata)
{
    [[@"changed" dataUsingEncoding:NSUTF8StringEncoding] writeToFile:@((directory + "/" + name).c_str())
                                                          atomically:YES];
    EXPECT_FALSE(publish());
    EXPECT_EQ(record(), nil);
}

TEST_F(Reply, FailedSourceWriteCannotCommitMetadata)
{
    const auto path = directory + "/" + name.substr(0, 64) + ".request";
    ASSERT_TRUE([[NSFileManager defaultManager] createDirectoryAtPath:@(path.c_str())
                                          withIntermediateDirectories:YES
                                                           attributes:nil
                                                                error:nil]);
    EXPECT_FALSE(publish());
    EXPECT_EQ(record(), nil);
}

TEST_F(Reply, InvalidIdentityCannotCommitMetadata)
{
    EXPECT_FALSE(publish("../escape"));
    uuid.fill(0);
    EXPECT_FALSE(publish());
    EXPECT_EQ(record(), nil);
}

TEST_F(Reply, ConcurrentSameContentPublishesWholeMetadata)
{
    std::array<std::thread, 8> threads;
    for (auto &thread : threads)
        thread = std::thread([&] {
            @autoreleasepool {
                EXPECT_TRUE(publish());
            }
        });
    for (auto &thread : threads)
        thread.join();
    ASSERT_NE(record(), nil);
    EXPECT_TRUE([record()[@"replySHA256"] isEqualToString:@(digest(reply.bytes, reply.length).c_str())]);
}
}  // namespace
