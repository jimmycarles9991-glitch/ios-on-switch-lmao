// SPDX-License-Identifier: GPL-2.0-only
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "bazel/testing/source_text.h"
#include "switch/prefetch_list.h"
#include "switch/prefetch_pace.h"
#include "switch/prefetch_quiesce.h"

using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

std::string bodyOf(const std::string &source, const char *signature)
{
    size_t searchFrom = 0;
    while (searchFrom < source.size()) {
        const size_t pos = source.find(signature, searchFrom);
        if (pos == std::string::npos) {
            return {};
        }
        const size_t open = source.find('{', pos);
        const size_t semi = source.find(';', pos);
        if (open == std::string::npos) {
            return {};
        }
        if (semi != std::string::npos && semi < open) {
            searchFrom = semi + 1;
            continue;
        }
        int depth = 0;
        for (size_t i = open; i < source.size(); ++i) {
            if (source[i] == '{') {
                ++depth;
            } else if (source[i] == '}') {
                --depth;
                if (depth == 0) {
                    return source.substr(open, i - open + 1);
                }
            }
        }
        return {};
    }
    return {};
}

struct switch_prefetch_header makeExpectedKey()
{
    struct switch_prefetch_header key = {};
    memcpy(key.magic, SWITCH_PREFETCH_LIST_V3_MAGIC, 8);
    key.version = SWITCH_PREFETCH_LIST_VERSION;
    key.header_bytes = sizeof(key);
    key.images[0] = {3303014400ULL, 0x1d2e3fULL, 1730800000, 123456789};
    key.images[1] = {10485760ULL, 0x4a5bULL, 1730800001, 987654321};
    for (unsigned i = 0; i < 16; ++i) {
        key.root_uuid[i] = static_cast<uint8_t>(0x10u + i);
        key.data_uuid[i] = static_cast<uint8_t>(0x80u + i);
    }
    return key;
}

std::vector<uint8_t> serializeList(struct switch_prefetch_header header,
                                   const std::vector<struct switch_prefetch_run> &runs)
{
    header.runs = static_cast<uint32_t>(runs.size());
    header.checksum =
        switch_prefetch_fnv(reinterpret_cast<const uint8_t *>(runs.data()), runs.size() * sizeof(runs[0]));
    std::vector<uint8_t> bytes(sizeof(header) + runs.size() * sizeof(runs[0]));
    memcpy(bytes.data(), &header, sizeof(header));
    if (!runs.empty()) {
        memcpy(bytes.data() + sizeof(header), runs.data(), runs.size() * sizeof(runs[0]));
    }
    return bytes;
}

struct FakeFs {
    std::map<std::string, std::vector<uint8_t>> files;
    int failWrite = 0;
    int failRename = 0;
    std::vector<std::string> writtenPaths;
    std::vector<std::pair<std::string, std::string>> renames;
    std::vector<std::string> unlinkedPaths;
};

int fakeWrite(void *ctx, const char *path, const void *header, size_t headerLen, const void *runs, size_t runsLen)
{
    auto *fs = static_cast<FakeFs *>(ctx);
    fs->writtenPaths.emplace_back(path);
    if (fs->failWrite != 0) {
        fs->files[path] = {0xde, 0xad};
        return fs->failWrite;
    }
    std::vector<uint8_t> buf(headerLen + runsLen);
    memcpy(buf.data(), header, headerLen);
    if (runsLen != 0u) {
        memcpy(buf.data() + headerLen, runs, runsLen);
    }
    fs->files[path] = std::move(buf);
    return 0;
}

int fakeRename(void *ctx, const char *from, const char *to)
{
    auto *fs = static_cast<FakeFs *>(ctx);
    fs->renames.emplace_back(from, to);
    if (fs->failRename != 0) {
        return fs->failRename;
    }
    auto it = fs->files.find(from);
    if (it == fs->files.end()) {
        return 2;
    }
    fs->files[to] = std::move(it->second);
    fs->files.erase(it);
    return 0;
}

void fakeUnlink(void *ctx, const char *path)
{
    auto *fs = static_cast<FakeFs *>(ctx);
    fs->unlinkedPaths.emplace_back(path);
    fs->files.erase(path);
}

}  // namespace

TEST(SwitchPrefetchList, ValidatesV3AndRefusesV2AndMismatches)
{
    const auto expected = makeExpectedKey();
    const std::vector<struct switch_prefetch_run> runs = {
        {0x1000123ULL, 4096ULL * 8192ULL, 1730800100, 55, 4096u, 128u, 3u, 0u},
        {0x1000123ULL, 4096ULL * 8192ULL, 1730800100, 55, 4224u, 17u, 9u, 1u},
    };
    const auto valid = serializeList(expected, runs);
    struct switch_prefetch_header parsed = {};
    EXPECT_EQ(switch_prefetch_validate_file(valid.data(), valid.size(), &expected, 1u << 18, &parsed), nullptr);
    EXPECT_EQ(parsed.runs, 2u);

    // Frozen SWCHPF02 list is recognized and refused with "another format".
    struct switch_list_header_v2 v2 = {};
    memcpy(v2.magic, SWITCH_PREFETCH_LIST_V2_MAGIC, 8);
    v2.version = 2u;
    v2.header_bytes = sizeof(v2);
    EXPECT_STREQ(
        switch_prefetch_validate_file(reinterpret_cast<const uint8_t *>(&v2), sizeof(v2), &expected, 1u << 18, nullptr),
        "another format");

    // Wrong magic, version, header size.
    auto bad = valid;
    bad[7] = '4';
    EXPECT_STREQ(switch_prefetch_validate_file(bad.data(), bad.size(), &expected, 1u << 18, nullptr),
                 "not an SWCHPF03 list");

    auto hdr = expected;
    hdr.version = 4u;
    auto badVer = serializeList(hdr, runs);
    EXPECT_STREQ(switch_prefetch_validate_file(badVer.data(), badVer.size(), &expected, 1u << 18, nullptr),
                 "an SWCHPF03 list of another version");

    // Changed cryptex image, root UUID, Data UUID.
    hdr = expected;
    hdr.images[1].mtime_nsec += 1;
    auto badImg = serializeList(hdr, runs);
    EXPECT_STREQ(switch_prefetch_validate_file(badImg.data(), badImg.size(), &expected, 1u << 18, nullptr),
                 "saved for other cryptex images");

    hdr = expected;
    hdr.root_uuid[0] ^= 1u;
    auto badRoot = serializeList(hdr, runs);
    EXPECT_STREQ(switch_prefetch_validate_file(badRoot.data(), badRoot.size(), &expected, 1u << 18, nullptr),
                 "saved for another root volume");

    hdr = expected;
    hdr.data_uuid[15] ^= 1u;
    auto badData = serializeList(hdr, runs);
    EXPECT_STREQ(switch_prefetch_validate_file(badData.data(), badData.size(), &expected, 1u << 18, nullptr),
                 "saved for another Data volume");

    // Torn short list and checksum mismatch.
    EXPECT_STREQ(switch_prefetch_validate_file(valid.data(), valid.size() - 4u, &expected, 1u << 18, nullptr),
                 "torn (short list)");
    auto badCsum = valid;
    badCsum.back() ^= 0xffu;
    EXPECT_STREQ(switch_prefetch_validate_file(badCsum.data(), badCsum.size(), &expected, 1u << 18, nullptr),
                 "torn (its runs do not match its checksum)");

    // Invalid run fields: bad volume, zero pages, over max run limit.
    auto badRuns = runs;
    badRuns[0].volume = SWITCH_PREFETCH_VOLUME_COUNT;
    auto badVol = serializeList(expected, badRuns);
    EXPECT_STREQ(switch_prefetch_validate_file(badVol.data(), badVol.size(), &expected, 1u << 18, nullptr),
                 "invalid run");

    badRuns = runs;
    badRuns[1].pages = 0u;
    auto badZero = serializeList(expected, badRuns);
    EXPECT_STREQ(switch_prefetch_validate_file(badZero.data(), badZero.size(), &expected, 1u << 18, nullptr),
                 "invalid run");

    badRuns = runs;
    badRuns[1].pages = SWITCH_PREFETCH_MAX_RUN_LIMIT + 1u;
    auto badBig = serializeList(expected, badRuns);
    EXPECT_STREQ(switch_prefetch_validate_file(badBig.data(), badBig.size(), &expected, 1u << 18, nullptr),
                 "invalid run");

    // Per-run file metadata validation.
    EXPECT_TRUE(switch_prefetch_run_matches_file(&runs[0], 4096ULL * 8192ULL, 1730800100, 55));
    EXPECT_FALSE(switch_prefetch_run_matches_file(&runs[0], 4096ULL * 8192ULL + 1u, 1730800100, 55));
    EXPECT_FALSE(switch_prefetch_run_matches_file(&runs[0], 4096ULL * 8192ULL, 1730800101, 55));
    EXPECT_FALSE(switch_prefetch_run_matches_file(&runs[0], 4096ULL * 8192ULL, 1730800100, 56));
}

TEST(SwitchPrefetchList, MergesDemandAndKeptRunsInFirstUseOrder)
{
    std::mt19937_64 rng(17);
    constexpr uint32_t kMaxRun = SWITCH_PREFETCH_MAX_RUN_PAGES;
    constexpr uint32_t kMaxGap = SWITCH_PREFETCH_MAX_GAP_PAGES;
    for (int round = 0; round < 400; ++round) {
        const uint32_t n = 1u + static_cast<uint32_t>(rng() % 200u);
        std::vector<struct switch_prefetch_range> ranges(n);
        uint64_t pages = 0;
        for (auto &r : ranges) {
            r.volume = static_cast<uint8_t>(rng() % SWITCH_PREFETCH_VOLUME_COUNT);
            r.file_id = 100u + (rng() % 6u);
            r.file_size = 4096ULL * 8192ULL;
            r.mtime_sec = 1700000000 + static_cast<int64_t>(r.file_id);
            r.mtime_nsec = 42;
            r.first = static_cast<uint32_t>(rng() % 3000u);
            r.end = r.first + 1u + static_cast<uint32_t>(rng() % ((rng() % 4u) ? 16u : 400u));
            r.when = static_cast<uint16_t>(1u + (rng() % 300u));
            pages += r.end - r.first;
        }
        std::sort(ranges.begin(), ranges.end(), [](const auto &x, const auto &y) {
            return switch_prefetch_compare_ranges(&x, &y) < 0;
        });
        const uint64_t cap = switch_prefetch_run_capacity(n, pages, kMaxRun, kMaxGap);
        std::vector<struct switch_prefetch_run> runs(cap);
        uint32_t files = 0;
        const uint32_t count = switch_prefetch_merge_runs(ranges.data(), n, runs.data(), cap, kMaxRun, kMaxGap, &files);
        ASSERT_LT(count, cap);
        std::set<std::pair<uint64_t, uint8_t>> distinct;
        for (const auto &r : ranges) {
            distinct.insert({r.file_id, r.volume});
        }
        EXPECT_EQ(files, distinct.size());
        for (uint32_t i = 0; i < count; ++i) {
            EXPECT_GE(runs[i].pages, 1u);
            EXPECT_LE(runs[i].pages, kMaxRun);
        }
        std::sort(runs.begin(), runs.begin() + count, [](const auto &x, const auto &y) {
            return switch_prefetch_compare_runs(&x, &y) < 0;
        });
        for (uint32_t i = 1; i < count; ++i) {
            EXPECT_LE(runs[i - 1].when, runs[i].when);
        }
    }

    const struct switch_prefetch_run capRuns[] = {
        {1, 1000, 0, 0, 0, 128, 1, 0},
        {1, 1000, 0, 0, 128, 128, 1, 0},
        {2, 1000, 0, 0, 0, 64, 2, 0},
        {3, 1000, 0, 0, 0, 128, 9, 1},
    };
    EXPECT_EQ(switch_prefetch_cap_runs(capRuns, 4, 448, 4), 4u);
    EXPECT_EQ(switch_prefetch_cap_runs(capRuns, 4, 448, 2), 2u);
    EXPECT_EQ(switch_prefetch_cap_runs(capRuns, 4, 447, 4), 3u);
    EXPECT_EQ(switch_prefetch_cap_runs(capRuns, 4, 319, 4), 2u);
    EXPECT_EQ(switch_prefetch_cap_runs(capRuns, 4, 0, 4), 0u);

    // Near UINT32_MAX page numbers merge without wrapping uint32_t arithmetic.
    const struct switch_prefetch_range highRanges[] = {
        {9, 1ULL << 44, 0, 0, UINT32_MAX - 10u, UINT32_MAX - 5u, 2u, 0u},
        {9, 1ULL << 44, 0, 0, UINT32_MAX - 3u, UINT32_MAX, 1u, 0u},
    };
    struct switch_prefetch_run highRuns[4] = {};
    uint32_t highFiles = 0;
    const uint32_t highCount = switch_prefetch_merge_runs(highRanges, 2u, highRuns, 4u, kMaxRun, kMaxGap, &highFiles);
    ASSERT_EQ(highCount, 1u);
    EXPECT_EQ(highFiles, 1u);
    EXPECT_EQ(highRuns[0].page, UINT32_MAX - 10u);
    EXPECT_EQ(highRuns[0].pages, 10u);
    EXPECT_EQ(highRuns[0].when, 1u);
}

TEST(SwitchPrefetchList, CapsSavedRunsAtMaxRunsSoNextBootAcceptsList)
{
    constexpr uint32_t kMaxRuns = 1u << 18;
    constexpr uint64_t kCapPages = (2ULL << 30) / 4096ULL;
    const uint32_t built = kMaxRuns + 128u;
    std::vector<struct switch_prefetch_run> runs(built);
    for (uint32_t i = 0; i < built; ++i) {
        runs[i] = {1000ULL + i, 4096ULL, 1730800000, 0, 0u, 1u, 1u, 0u};
    }
    // Total pages (262,272) sit well below kCapPages (524,288), so only the max_runs cap stops the list.
    const uint32_t count = switch_prefetch_cap_runs(runs.data(), built, kCapPages, kMaxRuns);
    ASSERT_EQ(count, kMaxRuns);
    runs.resize(count);

    const auto expected = makeExpectedKey();
    const auto saved = serializeList(expected, runs);
    struct switch_prefetch_header parsed = {};
    EXPECT_EQ(switch_prefetch_validate_file(saved.data(), saved.size(), &expected, kMaxRuns, &parsed), nullptr);
    EXPECT_EQ(parsed.runs, kMaxRuns);
}

TEST(SwitchPrefetchList, ParsesCanonicalUuidsAndRejectsMalformedOnes)
{
    uint8_t out[16] = {};
    ASSERT_TRUE(switch_prefetch_parse_uuid("01234567-89ab-CDEF-0123-456789abcdef", out));
    const uint8_t want[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                              0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    EXPECT_EQ(memcmp(out, want, sizeof(want)), 0);
    EXPECT_FALSE(switch_prefetch_parse_uuid(nullptr, out));
    EXPECT_FALSE(switch_prefetch_parse_uuid("01234567-89ab-CDEF-0123-456789abcde", out));
    EXPECT_FALSE(switch_prefetch_parse_uuid("01234567-89ab-CDEF-0123-456789abcdef0", out));
    EXPECT_FALSE(switch_prefetch_parse_uuid("01234567_89ab-CDEF-0123-456789abcdef", out));
    EXPECT_FALSE(switch_prefetch_parse_uuid("01234567-89ab-CDEF-0123-456789abcdeg", out));
}

TEST(SwitchPrefetchList, RunPageinAlignsToVmPageSizeAndFileEnd)
{
    std::mt19937_64 rng(5);
    for (uint64_t vm : {4096ULL, 16384ULL}) {
        for (int i = 0; i < 20000; ++i) {
            struct switch_prefetch_run run = {};
            run.page = static_cast<uint32_t>(rng() % 4096u);
            run.pages = static_cast<uint16_t>(1u + (rng() % 128u));
            const uint64_t file = 1u + (rng() % (20ULL << 20));
            const uint64_t first = static_cast<uint64_t>(run.page) * SWITCH_PREFETCH_LIST_PAGE;
            struct switch_prefetch_pagein in = {};
            const bool any = switch_prefetch_run_pagein(&run, SWITCH_PREFETCH_LIST_PAGE, vm, file, &in);
            ASSERT_EQ(any, first < file);
            if (!any) {
                continue;
            }
            const uint64_t last = std::min<uint64_t>(first + static_cast<uint64_t>(run.pages) * 4096ULL, file) - 1u;
            const uint64_t want = (first / vm) * vm;
            const uint64_t wantEnd = ((last / vm) + 1u) * vm;
            ASSERT_EQ(in.offset, want);
            ASSERT_EQ(in.offset + in.length, wantEnd);
        }
    }
}

TEST(SwitchPrefetchList, SavesAtomicallyByTempThenRename)
{
    const auto expected = makeExpectedKey();
    const std::vector<struct switch_prefetch_run> runs = {
        {7ULL, 65536ULL, 1700000000, 0, 0u, 16u, 1u, 0u},
    };
    auto hdr = expected;
    hdr.runs = 1u;
    hdr.files = 1u;
    hdr.checksum = switch_prefetch_fnv(reinterpret_cast<const uint8_t *>(runs.data()), sizeof(runs[0]));
    const struct switch_prefetch_file_ops ops = {fakeWrite, fakeRename, fakeUnlink};
    const char *path = "/private/preboot/Switch-Staging/sys.img.boot-prefetch";
    const char *uuid = "11223344-5566-7788-99AA-BBCCDDEEFF00";

    // Happy path: writes temporary file then renames over destination.
    FakeFs fs;
    fs.files[path] = {1, 2, 3};
    ASSERT_EQ(switch_prefetch_save_atomic(path, uuid, &hdr, runs.data(), 1u, &ops, &fs), 0);
    ASSERT_EQ(fs.writtenPaths.size(), 1u);
    EXPECT_EQ(fs.writtenPaths[0], std::string(path) + ".Switch-" + uuid);
    ASSERT_EQ(fs.renames.size(), 1u);
    EXPECT_EQ(fs.renames[0].first, fs.writtenPaths[0]);
    EXPECT_EQ(fs.renames[0].second, path);
    EXPECT_EQ(fs.files.count(fs.writtenPaths[0]), 0u);
    EXPECT_EQ(switch_prefetch_validate_file(fs.files[path].data(), fs.files[path].size(), &expected, 1024u, nullptr),
              nullptr);

    // Failed write: never renames, unlinks temporary, preserves existing list untouched.
    FakeFs failW;
    failW.files[path] = {9, 8, 7};
    failW.failWrite = 28;
    EXPECT_EQ(switch_prefetch_save_atomic(path, uuid, &hdr, runs.data(), 1u, &ops, &failW), 28);
    EXPECT_TRUE(failW.renames.empty());
    EXPECT_EQ(failW.files[path], (std::vector<uint8_t>{9, 8, 7}));
    EXPECT_EQ(failW.files.count(std::string(path) + ".Switch-" + uuid), 0u);

    // Failed rename: unlinks temporary, preserves existing list untouched.
    FakeFs failR;
    failR.files[path] = {4, 5, 6};
    failR.failRename = 5;
    EXPECT_EQ(switch_prefetch_save_atomic(path, uuid, &hdr, runs.data(), 1u, &ops, &failR), 5);
    EXPECT_EQ(failR.files[path], (std::vector<uint8_t>{4, 5, 6}));
    EXPECT_EQ(failR.files.count(std::string(path) + ".Switch-" + uuid), 0u);
}

TEST(SwitchPrefetchPace, EnforcesInflightAndLeadBoundsAndFirstReclaimClamp)
{
    struct switch_prefetch_pace p = {};
    switch_pace_init(&p, /*list_vm_pages=*/1000u, /*headroom_pages=*/64u, /*inflight_limit=*/3u * 512u * 1024u);
    EXPECT_EQ(p.lead_bound, 64u);
    EXPECT_TRUE(switch_pace_can_issue(&p));

    // In-flight bytes bound: 3 x 512 KiB blocks further issue until completion frees headroom.
    switch_pace_inflight_add(&p, 512u * 1024u);
    switch_pace_inflight_add(&p, 512u * 1024u);
    EXPECT_TRUE(switch_pace_can_issue(&p));
    switch_pace_inflight_add(&p, 512u * 1024u);
    EXPECT_FALSE(switch_pace_can_issue(&p));
    p.waiting = 1u;
    EXPECT_TRUE(switch_pace_inflight_done(&p, 512u * 1024u));
    EXPECT_TRUE(switch_pace_can_issue(&p));
    p.waiting = 0u;
    (void)switch_pace_inflight_done(&p, 1024u * 1024u);

    // Lead bound: reaching lead == lead_bound pauses replay until a page is used.
    switch_pace_committed(&p, 64u);
    EXPECT_EQ(switch_pace_lead(&p), 64u);
    EXPECT_FALSE(switch_pace_can_issue(&p));
    p.waiting = 1u;
    EXPECT_TRUE(switch_pace_on_used(&p));
    EXPECT_EQ(switch_pace_lead(&p), 63u);
    EXPECT_TRUE(switch_pace_can_issue(&p));

    // First reclaim of an unused replayed page clamps lead_bound to current lead and never loosens it.
    switch_pace_init(&p, 1000u, 200u, 3u * 512u * 1024u);
    switch_pace_committed(&p, 50u);
    (void)switch_pace_on_used(&p);
    // Lead is 49; reclaim takes it to 48 and clamps lead_bound to 48.
    (void)switch_pace_on_reclaimed(&p);
    EXPECT_EQ(p.lead_bound, 48u);
    EXPECT_FALSE(switch_pace_can_issue(&p));
    // A second reclaim drops lead to 47 without changing the already-clamped bound of 48.
    p.waiting = 1u;
    EXPECT_TRUE(switch_pace_on_reclaimed(&p));
    EXPECT_EQ(p.lead_bound, 48u);
    EXPECT_TRUE(switch_pace_can_issue(&p));

    // Zero headroom disables replay immediately and leaves hooks inactive.
    struct switch_prefetch_pace tight = {};
    switch_pace_init(&tight, 500u, 0u, 3u * 512u * 1024u);
    EXPECT_EQ(tight.lead_bound, 0u);
    EXPECT_FALSE(switch_pace_can_issue(&tight));
    EXPECT_FALSE(switch_pace_on_used(&tight));
    EXPECT_EQ(tight.used_pages, 0u);

    // Deactivation suppresses post-boot accounting and wakeups completely.
    switch_pace_deactivate(&p);
    p.waiting = 1u;
    const uint64_t usedBefore = p.used_pages;
    const uint64_t reclaimedBefore = p.reclaimed_pages;
    EXPECT_FALSE(switch_pace_on_used(&p));
    EXPECT_FALSE(switch_pace_on_reclaimed(&p));
    EXPECT_EQ(p.used_pages, usedBefore);
    EXPECT_EQ(p.reclaimed_pages, reclaimedBefore);

    // Draining inflight_bytes to 0 wakes a waiter even when lead is at lead_bound.
    struct switch_prefetch_pace drain = {};
    switch_pace_init(&drain, 100u, 10u, 3u * 512u * 1024u);
    switch_pace_committed(&drain, 10u);
    switch_pace_inflight_add(&drain, 512u * 1024u);
    EXPECT_FALSE(switch_pace_can_issue(&drain));
    drain.waiting = 1u;
    EXPECT_TRUE(switch_pace_inflight_done(&drain, 512u * 1024u));
    EXPECT_EQ(switch_pace_inflight_bytes(&drain), 0u);
}

TEST(SwitchPrefetchPace, StopEventsReleaseWaitingReplayAndFirstWins)
{
    // Pressure stops replay immediately without ending boot recording, so Boot Complete can still save a trimmed list.
    {
        struct switch_prefetch_pace p = {};
        switch_pace_init(&p, 500u, 100u, 3u * 512u * 1024u);
        p.waiting = 1u;
        uint32_t first = 0u;
        EXPECT_TRUE(switch_pace_stop_replay(&p, 1u, &first));
        EXPECT_EQ(first, 1u);
        EXPECT_EQ(switch_pace_replay_stop_reason(&p), 1u);
        EXPECT_EQ(switch_pace_boot_stop_reason(&p), 0u);
        EXPECT_FALSE(switch_pace_can_issue(&p));

        uint32_t bootFirst = 0u;
        EXPECT_TRUE(switch_pace_stop_boot(&p, 3u, &bootFirst));
        EXPECT_EQ(bootFirst, 3u);
        EXPECT_EQ(switch_pace_boot_stop_reason(&p), 3u);
        EXPECT_EQ(switch_pace_replay_stop_reason(&p), 1u);
    }

    for (uint32_t reason : {2u, 3u}) {
        struct switch_prefetch_pace p = {};
        switch_pace_init(&p, 500u, 100u, 3u * 512u * 1024u);
        p.waiting = 1u;
        uint32_t first = 0u;
        EXPECT_TRUE(switch_pace_stop_boot(&p, reason, &first));
        EXPECT_EQ(first, reason);
        EXPECT_EQ(switch_pace_boot_stop_reason(&p), reason);
        EXPECT_EQ(switch_pace_replay_stop_reason(&p), reason);
        EXPECT_FALSE(switch_pace_can_issue(&p));

        uint32_t second = 0u;
        (void)switch_pace_stop_boot(&p, reason == 2u ? 3u : 2u, &second);
        EXPECT_EQ(second, reason);
        EXPECT_EQ(switch_pace_boot_stop_reason(&p), reason);
    }
}

TEST(SwitchPrefetchQuiesce, StopWaitsForActiveWriterBeforeFree)
{
    struct WaitState {
        std::mutex mu;
        std::condition_variable cv;
        bool woke = false;
    } state;

    auto wake = [](void *arg) {
        auto *s = static_cast<WaitState *>(arg);
        std::lock_guard<std::mutex> lock(s->mu);
        s->woke = true;
        s->cv.notify_all();
    };
    auto wait = [](struct switch_quiesce *q, void *arg) {
        auto *s = static_cast<WaitState *>(arg);
        std::unique_lock<std::mutex> lock(s->mu);
        for (;;) {
            s->woke = false;
            if (!switch_quiesce_mark_stopping(q)) {
                break;
            }
            s->cv.wait(lock, [&] {
                return s->woke || !switch_quiesce_has_writers(q);
            });
        }
    };

    struct switch_quiesce q;
    switch_quiesce_init(&q);
    int logBuf = 42;
    switch_quiesce_start(&q, &logBuf);

    void *entered = switch_quiesce_enter(&q, wake, &state);
    ASSERT_EQ(entered, &logBuf);

    std::atomic<bool> stopDone{false};
    std::thread stopper([&] {
        void *old = switch_quiesce_stop(&q, wait, &state);
        EXPECT_EQ(old, &logBuf);
        stopDone.store(true, std::memory_order_release);
    });

    // Spin briefly until stopper has cleared ptr and entered waiting state.
    while (switch_quiesce_is_armed(&q)) {
        std::this_thread::yield();
    }
    EXPECT_FALSE(stopDone.load(std::memory_order_acquire));

    // A late writer entering after stop sees NULL and never touches logBuf.
    EXPECT_EQ(switch_quiesce_enter(&q, wake, &state), nullptr);
    EXPECT_FALSE(stopDone.load(std::memory_order_acquire));

    // Once the active writer leaves, stopper wakes and completes.
    switch_quiesce_leave(&q, wake, &state);
    stopper.join();
    EXPECT_TRUE(stopDone.load(std::memory_order_acquire));
}

TEST(SwitchPrefetchSource, BothXnuTreesHoldVmHooksWithoutWakingUnderLocks)
{
    struct Tree {
        const char *name;
        const char *pageStructHdr;
        const char *pageMacroHdr;
        const char *objHdr;
        const char *commitFile;
    };

    const Tree trees[] = {
        {"xnu-10063.141.1", "vm/vm_page.h", "vm/vm_page.h", "vm/vm_object.h", "vm/vm_pageout.c"},
        {"xnu-12377.121.6", "vm/vm_page.h", "vm/vm_page_internal.h", "vm/vm_object_xnu.h", "vm/vm_upl.c"},
    };
    for (const auto &t : trees) {
        const std::string pageStructHdr = SourceText(std::string("../") + t.name + "/osfmk/" + t.pageStructHdr);
        const std::string pageMacroHdr = SourceText(std::string("../") + t.name + "/osfmk/" + t.pageMacroHdr);
        const std::string objHdr = SourceText(std::string("../") + t.name + "/osfmk/" + t.objHdr);
        const std::string pageout = SourceText(std::string("../") + t.name + "/osfmk/vm/vm_pageout.c");
        const std::string commit = SourceText(std::string("../") + t.name + "/osfmk/" + t.commitFile);
        const std::string resident = SourceText(std::string("../") + t.name + "/osfmk/vm/vm_resident.c");
        const std::string memobj = SourceText(std::string("../") + t.name + "/osfmk/vm/memory_object.c");
        const std::string fault = SourceText(std::string("../") + t.name + "/osfmk/vm/vm_fault.c");
        const std::string cluster = SourceText(std::string("../") + t.name + "/bsd/vfs/vfs_cluster.c");

        // Hook 1: per-page vmp_switch_prefetched bit (not sticky per-object); no wake inside macro.
        EXPECT_THAT(pageStructHdr, HasSubstr("vmp_switch_prefetched:1")) << t.name;
        EXPECT_THAT(objHdr, Not(HasSubstr("switch_prefetched"))) << t.name;
        EXPECT_THAT(objHdr, HasSubstr("switch_wake:1")) << t.name;
        const size_t macroPos = pageMacroHdr.find("#define VM_PAGE_CONSUME_CLUSTERED_WAKE");
        ASSERT_NE(macroPos, std::string::npos) << t.name;
        const size_t macroEnd = pageMacroHdr.find("#define VM_PAGE_CONSUME_CLUSTERED(", macroPos);
        ASSERT_NE(macroEnd, std::string::npos) << t.name;
        const std::string macro = pageMacroHdr.substr(macroPos, macroEnd - macroPos);
        EXPECT_THAT(macro, HasSubstr("mem->vmp_switch_prefetched")) << t.name;
        EXPECT_THAT(macro, Not(HasSubstr("o->switch_prefetched"))) << t.name;
        EXPECT_THAT(macro, HasSubstr("switch_vm_prefetch_used_locked()")) << t.name;
        EXPECT_THAT(macro, Not(HasSubstr("switch_vm_prefetch_wake"))) << t.name;

        // Per-page bit is set in vm_page_do_delayed_work and cleared on free/reclaim.
        EXPECT_THAT(commit, HasSubstr("DW_vm_page_switch_prefetched")) << t.name;
        EXPECT_THAT(bodyOf(resident, "vm_page_do_delayed_work("), HasSubstr("m->vmp_switch_prefetched = TRUE;"))
            << t.name;
        EXPECT_THAT(bodyOf(resident, "vm_page_free_prepare_queues("), HasSubstr("mem->vmp_switch_prefetched = FALSE;"))
            << t.name;

        // Hook 2: reclaim_page tests and clears per-page vmp_switch_prefetched; wake runs after unlock.
        const size_t reclaimPos = pageout.find("reclaim_page:");
        ASSERT_NE(reclaimPos, std::string::npos) << t.name;
        const std::string reclaimWindow = pageout.substr(reclaimPos, 1200);
        EXPECT_THAT(reclaimWindow, HasSubstr("m->vmp_switch_prefetched")) << t.name;
        EXPECT_THAT(reclaimWindow, HasSubstr("m->vmp_switch_prefetched = FALSE;")) << t.name;
        EXPECT_THAT(reclaimWindow, HasSubstr("switch_vm_prefetch_reclaimed_locked()")) << t.name;
        EXPECT_THAT(reclaimWindow, HasSubstr("switch_wake = true;")) << t.name;
        EXPECT_THAT(reclaimWindow, Not(HasSubstr("switch_vm_prefetch_wake"))) << t.name;

        const std::string prep = bodyOf(pageout, "vm_pageout_prepare_to_block(");
        ASSERT_FALSE(prep.empty()) << t.name;
        const size_t unlockQ = prep.find("vm_page_unlock_queues();");
        const size_t unlockObj = prep.find("vm_object_unlock(*object);");
        const size_t wakePos = prep.find("switch_vm_prefetch_wake") != std::string::npos
                                   ? prep.find("switch_vm_prefetch_wake")
                                   : prep.find("SWITCH_VM_PREFETCH_FLUSH_WAKE");
        ASSERT_NE(unlockQ, std::string::npos) << t.name;
        ASSERT_NE(unlockObj, std::string::npos) << t.name;
        ASSERT_NE(wakePos, std::string::npos) << t.name;
        EXPECT_LT(unlockQ, wakePos) << t.name;
        EXPECT_LT(unlockObj, wakePos) << t.name;

        // Pressure stop hook and UPL pacing hooks present; exactly two switch_committed flushes in upl_commit_range.
        EXPECT_THAT(bodyOf(pageout, "vm_pressure_response("), HasSubstr("switch_vm_pressure_changed(")) << t.name;
        EXPECT_THAT(bodyOf(pageout, "upl_deallocate("), HasSubstr("switch_vm_prefetch_inflight_done(")) << t.name;
        size_t committedCalls = 0;
        for (size_t pos = 0; (pos = commit.find("switch_vm_prefetch_committed(", pos)) != std::string::npos;
             pos += 29) {
            ++committedCalls;
        }
        EXPECT_EQ(committedCalls, 2u) << t.name;

        // PG5: vm_object_update flushes switch_wake even when copy_object == object; copy-object loop returns flush.
        const std::string objUpdate = bodyOf(memobj, "vm_object_update(");
        EXPECT_THAT(objUpdate, HasSubstr("else if (__improbable(switch_wake))")) << t.name;
        const std::string faultPage = bodyOf(fault, "vm_fault_page(");
        const size_t copyDealloc = faultPage.find("vm_object_deallocate(copy_object);");
        ASSERT_NE(copyDealloc, std::string::npos) << t.name;
        const size_t copyRetry = faultPage.find("return VM_FAULT_RETRY;", copyDealloc);
        ASSERT_NE(copyRetry, std::string::npos) << t.name;
        EXPECT_THAT(faultPage.substr(copyRetry > 140 ? copyRetry - 140 : 0, 140),
                    HasSubstr("SWITCH_VM_PREFETCH_FLUSH_WAKE(fault_info->switch_wake)"))
            << t.name;
        const size_t copyShortage = faultPage.find("return VM_FAULT_MEMORY_SHORTAGE;", copyRetry);
        ASSERT_NE(copyShortage, std::string::npos) << t.name;
        EXPECT_THAT(faultPage.substr(copyShortage > 140 ? copyShortage - 140 : 0, 140),
                    HasSubstr("SWITCH_VM_PREFETCH_FLUSH_WAKE(fault_info->switch_wake)"))
            << t.name;

        // Recording log uses quiesce gate and skips the replay thread.
        EXPECT_THAT(cluster, HasSubstr("switch_quiesce_enter(")) << t.name;
        EXPECT_THAT(cluster, HasSubstr("switch_quiesce_leave(")) << t.name;
        EXPECT_THAT(cluster, HasSubstr("switch_quiesce_stop(")) << t.name;
        EXPECT_THAT(cluster, HasSubstr("switch_record_skip_thread")) << t.name;
    }

    // PG5 (iOS 17): vm_fault_wire_fast GIVE_UP flushes fault_info.switch_wake after UNLOCK_AND_DEALLOCATE.
    const std::string fault17 = SourceText("../xnu-10063.141.1/osfmk/vm/vm_fault.c");
    const std::string wireFast17 = bodyOf(fault17, "vm_fault_wire_fast(");
    const size_t giveUpPos = wireFast17.find("#define GIVE_UP");
    ASSERT_NE(giveUpPos, std::string::npos);
    EXPECT_THAT(wireFast17.substr(giveUpPos, 200), HasSubstr("SWITCH_VM_PREFETCH_FLUSH_WAKE(fault_info.switch_wake)"));

    // PG6 (iOS 27): replay thread bypasses cluster_handle_split_pagein so >32 KiB runs stay unsplit DMA reads.
    const std::string cluster27 = SourceText("../xnu-12377.121.6/bsd/vfs/vfs_cluster.c");
    const std::string pageinExt27 = bodyOf(cluster27, "cluster_pagein_ext(");
    const size_t splitPos = pageinExt27.find("cluster_handle_split_pagein(");
    ASSERT_NE(splitPos, std::string::npos);
    EXPECT_THAT(pageinExt27.substr(splitPos > 180 ? splitPos - 180 : 0, 180), HasSubstr("switch_record_skip_thread"));
}

TEST(SwitchPrefetchSource, ReplayReleasesVnodeDrainsInflightAndUsesLowPriority)
{
    const std::string prefetch = SourceText("../switch/vm/prefetch.c");
    const std::string prefetchVm = SourceText("../switch/vm/prefetch_vm.c");
    const std::string prefetchSave = SourceText("../switch/vm/prefetch_save.c");

    const std::string replay = bodyOf(prefetch, "switch_prefetch_replay_runs(");
    ASSERT_FALSE(replay.empty());
    EXPECT_THAT(replay, HasSubstr("vnode_pagein(vp, NULL, 0, in.offset,"));
    EXPECT_THAT(replay, Not(HasSubstr("advisory_read")));
    const size_t putPos = replay.find("vnode_put(vp);");
    const size_t waitPos = replay.find("switch_vm_prefetch_wait_can_issue()");
    ASSERT_NE(putPos, std::string::npos);
    ASSERT_NE(waitPos, std::string::npos);
    EXPECT_LT(putPos, waitPos);
    EXPECT_THAT(replay, HasSubstr("switch_vm_prefetch_wait_inflight_drain();"));

    // PG2: Dekker startup-vs-shutdown protocol sets thread_active (seq_cst) before checking boot stop.
    const std::string startBody = bodyOf(prefetch, "switch_prefetch_start(");
    const size_t activePos = startBody.find("g_prefetch.thread_active, 1u, __ATOMIC_SEQ_CST");
    const size_t stopCheckPos = startBody.find("switch_vm_prefetch_boot_stop_reason()");
    ASSERT_NE(activePos, std::string::npos);
    ASSERT_NE(stopCheckPos, std::string::npos);
    EXPECT_LT(activePos, stopCheckPos);
    EXPECT_THAT(bodyOf(prefetch, "switch_cryptex_mounted("), HasSubstr("switch_prefetch_start();"));
    EXPECT_THAT(bodyOf(prefetch, "switch_prefetch_volumes_mounted("), HasSubstr("switch_prefetch_start();"));
    EXPECT_THAT(bodyOf(prefetch, "switch_prefetch_shutdown("), HasSubstr("g_prefetch.thread_active, __ATOMIC_SEQ_CST"));

    // PG3: debug diag_call uses THREAD_CALL_OPTIONS_ONCE, sets diag_stop before cancel_wait/free, then clears pointer.
    const std::string threadBody = bodyOf(prefetch, "switch_prefetch_thread(");
    EXPECT_THAT(threadBody, HasSubstr("THREAD_CALL_OPTIONS_ONCE"));
    const size_t diagStopPos = threadBody.find("g_prefetch.diag_stop, 1u, __ATOMIC_SEQ_CST");
    const size_t cancelWaitPos = threadBody.find("thread_call_cancel_wait(call)");
    const size_t freePos = threadBody.find("while (!thread_call_free(call))");
    const size_t clearCallPos = threadBody.find("g_prefetch.diag_call = NULL;");
    ASSERT_NE(diagStopPos, std::string::npos);
    ASSERT_NE(cancelWaitPos, std::string::npos);
    ASSERT_NE(freePos, std::string::npos);
    ASSERT_NE(clearCallPos, std::string::npos);
    EXPECT_LT(diagStopPos, cancelWaitPos);
    EXPECT_LT(cancelWaitPos, freePos);
    EXPECT_LT(freePos, clearCallPos);

    // PG7: replay/save thread runs at BASEPRI_UTILITY, never BASEPRI_KERNEL.
    const std::string startThread = bodyOf(prefetchVm, "switch_vm_prefetch_start_thread(");
    EXPECT_THAT(startThread, HasSubstr("kernel_thread_start_priority("));
    EXPECT_THAT(startThread, HasSubstr("BASEPRI_UTILITY"));

    // PG8: atomic save fsyncs temp file before rename and unlinks with VNODE_REMOVE_NOFOLLOW_ANY.
    const std::string writeFile = bodyOf(prefetchSave, "kernel_write_file(");
    const size_t fsyncPos = writeFile.find("VNOP_FSYNC(vp, MNT_WAIT, ctx)");
    const size_t closePos = writeFile.find("vnode_close(");
    ASSERT_NE(fsyncPos, std::string::npos);
    ASSERT_NE(closePos, std::string::npos);
    EXPECT_LT(fsyncPos, closePos);
    EXPECT_THAT(bodyOf(prefetchSave, "kernel_unlink_file("), HasSubstr("VNODE_REMOVE_NOFOLLOW_ANY"));

    // Shutdown hook runs in both debug and release branches of shutdown.c.
    const std::string shutdown = SourceText("../switch/shutdown/shutdown.c");
    size_t count = 0;
    for (size_t pos = 0; (pos = shutdown.find("switch_prefetch_shutdown()", pos)) != std::string::npos; pos += 26) {
        ++count;
    }
    EXPECT_GE(count, 2u);
}

TEST(SwitchPrefetchList, RefusesZeroVolumeUuidsAndInvalidRunIdentity)
{
    const auto expected = makeExpectedKey();
    const std::vector<struct switch_prefetch_run> allFour = {
        {0x101ULL, 65536ULL, 1730800100, 1, 0u, 16u, 1u, 0u},
        {0x102ULL, 65536ULL, 1730800101, 2, 0u, 16u, 2u, 1u},
        {0x103ULL, 131072ULL, 1730800102, 3, 4u, 28u, 3u, 2u},
        {0x104ULL, 262144ULL, 1730800103, 4, 8u, 56u, 4u, 3u},
    };
    const auto valid = serializeList(expected, allFour);
    struct switch_prefetch_header parsed = {};
    EXPECT_EQ(switch_prefetch_validate_file(valid.data(), valid.size(), &expected, 1u << 18, &parsed), nullptr);
    EXPECT_EQ(parsed.runs, 4u);

    // Legacy zero-UUID headers never match even when both saved and expected carry zeroes.
    auto zeroRoot = expected;
    memset(zeroRoot.root_uuid, 0, sizeof(zeroRoot.root_uuid));
    const auto zeroRootBytes = serializeList(zeroRoot, allFour);
    EXPECT_STREQ(
        switch_prefetch_validate_file(zeroRootBytes.data(), zeroRootBytes.size(), &zeroRoot, 1u << 18, nullptr),
        "saved for another root volume");

    auto zeroData = expected;
    memset(zeroData.data_uuid, 0, sizeof(zeroData.data_uuid));
    const auto zeroDataBytes = serializeList(zeroData, allFour);
    EXPECT_STREQ(
        switch_prefetch_validate_file(zeroDataBytes.data(), zeroDataBytes.size(), &zeroData, 1u << 18, nullptr),
        "saved for another Data volume");

    // Zero file_id or zero file_size runs are rejected by both validate_file and validate_runs.
    auto zeroIdRuns = allFour;
    zeroIdRuns[2].file_id = 0u;
    const auto zeroIdBytes = serializeList(expected, zeroIdRuns);
    EXPECT_STREQ(switch_prefetch_validate_file(zeroIdBytes.data(), zeroIdBytes.size(), &expected, 1u << 18, nullptr),
                 "invalid run");

    auto zeroSizeRuns = allFour;
    zeroSizeRuns[3].file_size = 0u;
    const auto zeroSizeBytes = serializeList(expected, zeroSizeRuns);
    EXPECT_STREQ(
        switch_prefetch_validate_file(zeroSizeBytes.data(), zeroSizeBytes.size(), &expected, 1u << 18, nullptr),
        "invalid run");
}

TEST(SwitchPrefetchList, ReadyMaskStartsStrictlyOnceAcrossAllFourUniqueVolumes)
{
    int m0 = 0, m1 = 1, m2 = 2, m3 = 3, m4 = 4;

    // Normal boot order: System cryptex, App cryptex, root, Data.
    struct switch_prefetch_ready r = {};
    switch_prefetch_ready_init(&r);
    EXPECT_FALSE(switch_prefetch_ready_add(&r, 0u, nullptr));
    EXPECT_FALSE(switch_prefetch_ready_add(&r, SWITCH_PREFETCH_VOLUME_COUNT, &m0));
    EXPECT_FALSE(switch_prefetch_ready_add(&r, 0u, &m0));
    EXPECT_EQ(r.mask, 1u);
    EXPECT_FALSE(switch_prefetch_ready_add(&r, 0u, &m4));  // duplicate slot rejected
    EXPECT_EQ(r.mounts[0], &m0);
    EXPECT_FALSE(switch_prefetch_ready_add(&r, 1u, &m0));  // duplicate mount pointer rejected
    EXPECT_EQ(r.mask, 1u);
    EXPECT_EQ(r.mounts[1], nullptr);
    EXPECT_FALSE(switch_prefetch_ready_add(&r, 1u, &m1));
    EXPECT_FALSE(switch_prefetch_ready_add(&r, 2u, &m2));
    EXPECT_FALSE(switch_prefetch_ready_add(&r, 3u, &m2));  // duplicate on final slot must not trigger start
    EXPECT_EQ(r.mask, 0x7u);
    EXPECT_TRUE(switch_prefetch_ready_add(&r, 3u, &m3));
    EXPECT_EQ(r.mask, SWITCH_PREFETCH_READY_ALL);
    EXPECT_FALSE(switch_prefetch_ready_add(&r, 3u, &m4));  // fires strictly once

    // Reversed order (root + Data before cryptexes) also starts strictly once on the 4th mount.
    struct switch_prefetch_ready rev = {};
    switch_prefetch_ready_init(&rev);
    EXPECT_FALSE(switch_prefetch_ready_add(&rev, 2u, &m2));
    EXPECT_FALSE(switch_prefetch_ready_add(&rev, 3u, &m3));
    EXPECT_FALSE(switch_prefetch_ready_add(&rev, 0u, &m0));
    EXPECT_TRUE(switch_prefetch_ready_add(&rev, 1u, &m1));
    EXPECT_EQ(rev.mask, SWITCH_PREFETCH_READY_ALL);
}

TEST(SwitchPrefetchList, VnodeEligibilityRefusesDeletedAndSpecialVnodes)
{
    EXPECT_TRUE(switch_prefetch_vnode_eligible(true, false, false, false, false, true, 42u, 42u, 4096u, true, 1u));
    EXPECT_TRUE(switch_prefetch_vnode_eligible(true, false, false, false, false, true, 42u, 0u, 4096u, false, 0u));

    EXPECT_FALSE(switch_prefetch_vnode_eligible(false, false, false, false, false, true, 42u, 42u, 4096u, true, 1u));
    EXPECT_FALSE(switch_prefetch_vnode_eligible(true, true, false, false, false, true, 42u, 42u, 4096u, true, 1u));
    EXPECT_FALSE(switch_prefetch_vnode_eligible(true, false, true, false, false, true, 42u, 42u, 4096u, true, 1u));
    EXPECT_FALSE(switch_prefetch_vnode_eligible(true, false, false, true, false, true, 42u, 42u, 4096u, true, 1u));
    EXPECT_FALSE(switch_prefetch_vnode_eligible(true, false, false, false, true, true, 42u, 42u, 4096u, true, 1u));
    EXPECT_FALSE(switch_prefetch_vnode_eligible(true, false, false, false, false, false, 42u, 42u, 4096u, true, 1u));
    EXPECT_FALSE(switch_prefetch_vnode_eligible(true, false, false, false, false, true, 0u, 0u, 4096u, true, 1u));
    EXPECT_FALSE(switch_prefetch_vnode_eligible(true, false, false, false, false, true, 43u, 42u, 4096u, true, 1u));
    EXPECT_FALSE(switch_prefetch_vnode_eligible(true, false, false, false, false, true, 42u, 42u, 0u, true, 1u));
    // Open-but-unlinked Data file (va_nlink == 0) is rejected.
    EXPECT_FALSE(switch_prefetch_vnode_eligible(true, false, false, false, false, true, 42u, 42u, 4096u, true, 0u));
}

TEST(SwitchPrefetchList, ReplayedBitmaskTracksPerRunSuccess)
{
    uint8_t bits[64] = {};
    const uint32_t setIndices[] = {0u, 7u, 8u, 15u, 16u, 127u, 255u, 511u};
    for (uint32_t idx : setIndices) {
        switch_prefetch_bit_set(bits, idx);
    }
    for (uint32_t i = 0; i < 512u; ++i) {
        const bool expected = std::find(std::begin(setIndices), std::end(setIndices), i) != std::end(setIndices);
        EXPECT_EQ(switch_prefetch_bit_test(bits, i), expected) << i;
    }
}

TEST(SwitchPrefetchSource, RootAndDataVolumesWiredAcrossBothXnuTrees)
{
    for (const char *tree : {"xnu-10063.141.1", "xnu-12377.121.6"}) {
        const std::string cluster = SourceText(std::string("../") + tree + "/bsd/vfs/vfs_cluster.c");
        const std::string clusterIo = bodyOf(cluster, "cluster_io(");
        EXPECT_THAT(clusterIo, HasSubstr("switch_cluster_volume_index(mp)")) << tree;
        // SB-2: RELEASE short-circuits on switch_quiesce_is_armed before scanning the 4 volume slots.
        const size_t armedGatePos = clusterIo.find("__improbable(switch_quiesce_is_armed(&switch_record_gate))");
        const size_t volIdxPos = clusterIo.find("switch_cluster_volume_index(mp)");
        ASSERT_NE(armedGatePos, std::string::npos) << tree;
        ASSERT_NE(volIdxPos, std::string::npos) << tree;
        EXPECT_LT(armedGatePos, volIdxPos) << tree;
        EXPECT_THAT(bodyOf(cluster, "switch_cluster_is_cryptex_mount("), HasSubstr("switch_cluster_cryptex_index(mp)"))
            << tree;
        const std::string cryptexIdx = bodyOf(cluster, "switch_cluster_cryptex_index(");
        EXPECT_THAT(cryptexIdx, HasSubstr("SWITCH_PREFETCH_VOL_SYSTEM")) << tree;
        EXPECT_THAT(cryptexIdx, HasSubstr("SWITCH_PREFETCH_VOL_APP")) << tree;
        EXPECT_THAT(cryptexIdx, Not(HasSubstr("SWITCH_PREFETCH_VOL_ROOT"))) << tree;
        EXPECT_THAT(cryptexIdx, Not(HasSubstr("SWITCH_PREFETCH_VOL_DATA"))) << tree;
        EXPECT_THAT(bodyOf(cluster, "switch_cluster_record("), HasSubstr("VISNAMEDSTREAM | VSYSTEM | VSWAP")) << tree;

        const std::string exec = SourceText(std::string("../") + tree + "/bsd/kern/kern_exec.c");
        const std::string dataPrep = bodyOf(exec, "native_owner_data_prepare(");
        EXPECT_THAT(dataPrep, HasSubstr("native_owner_data_mount = data_mount;")) << tree;
        const std::string init = bodyOf(exec, "load_init_program(");
        const size_t policyPos = init.find("native_switch_watchdog_policy(");
        const size_t mountHookPos =
            init.find("switch_prefetch_volumes_mounted(vnode_mount(rootvnode), native_owner_data_mount);");
        const size_t launchdPos = init.find("load_init_program_at_path(");
        EXPECT_NE(policyPos, std::string::npos) << tree;
        EXPECT_NE(mountHookPos, std::string::npos) << tree;
        EXPECT_NE(launchdPos, std::string::npos) << tree;
        if (policyPos != std::string::npos && mountHookPos != std::string::npos && launchdPos != std::string::npos) {
            EXPECT_LT(policyPos, mountHookPos) << tree;
            EXPECT_LT(mountHookPos, launchdPos) << tree;
        }
    }

    const std::string prefetch = SourceText("../switch/vm/prefetch.c");
    const std::string prefetchSave = SourceText("../switch/vm/prefetch_save.c");

    const std::string volsMounted = bodyOf(prefetch, "switch_prefetch_volumes_mounted(");
    EXPECT_THAT(volsMounted, HasSubstr("SWITCH_PREFETCH_VOL_ROOT"));
    EXPECT_THAT(volsMounted, HasSubstr("SWITCH_PREFETCH_VOL_DATA"));
    EXPECT_THAT(volsMounted, HasSubstr("\"apfs\""));
    EXPECT_THAT(volsMounted, HasSubstr("MNT_RDONLY"));
    EXPECT_THAT(volsMounted, HasSubstr("switch_prefetch_mount_at(SWITCH_PREFETCH_VOL_ROOT) != NULL"));

    const std::string expKey = bodyOf(prefetch, "switch_prefetch_expected_key(");
    EXPECT_THAT(expKey, HasSubstr("SWITCH_OS_VOLUME_UUID"));
    EXPECT_THAT(expKey, HasSubstr("key->root_uuid"));
    EXPECT_THAT(expKey, HasSubstr("key->data_uuid"));
    EXPECT_THAT(bodyOf(prefetch, "switch_prefetch_mount_uuid("), HasSubstr("VFSATTR_WANTED(&vfa, f_uuid)"));
    EXPECT_THAT(bodyOf(prefetch, "switch_prefetch_mount_uuid("), HasSubstr("switch_prefetch_busy_mount(mp, fsid)"));

    const std::string replay = bodyOf(prefetch, "switch_prefetch_replay_runs(");
    EXPECT_THAT(replay, Not(HasSubstr("SWITCH_PREFETCH_CRYPTEX_COUNT")));
    EXPECT_THAT(replay, HasSubstr("switch_prefetch_vget_regular("));
    EXPECT_THAT(replay, HasSubstr("switch_prefetch_vnode_meta("));
    EXPECT_THAT(replay, HasSubstr("rc == PAGER_SUCCESS"));
    EXPECT_THAT(replay, Not(HasSubstr("vfs_flags(mp)")));

    const std::string threadBody = bodyOf(prefetch, "switch_prefetch_thread(");
    EXPECT_THAT(threadBody, HasSubstr("SWITCH_PREFETCH_VOLUME_COUNT"));
    EXPECT_THAT(threadBody, Not(HasSubstr("SWITCH_PREFETCH_CRYPTEX_COUNT")));

    // SB-1: all mount busy calls validate mount liveness through mount_list_lookupby_fsid + mount_iterdrop.
    const std::string busyMount = bodyOf(prefetchSave, "switch_prefetch_busy_mount(");
    const size_t lookupPos = busyMount.find("mount_list_lookupby_fsid(&fsid, 0, 1)");
    const size_t busyCallPos = busyMount.find("vfs_busy(mp, LK_NOWAIT)");
    const size_t dropPos = busyMount.find("mount_iterdrop(mp);");
    ASSERT_NE(lookupPos, std::string::npos);
    ASSERT_NE(busyCallPos, std::string::npos);
    ASSERT_NE(dropPos, std::string::npos);
    EXPECT_LT(lookupPos, busyCallPos);
    EXPECT_LT(busyCallPos, dropPos);

    const std::string vgetReg = bodyOf(prefetchSave, "switch_prefetch_vget_regular(");
    const size_t busyPos = vgetReg.find("switch_prefetch_busy_mount(mp, fsid)");
    const size_t vgetPos = vgetReg.find("VFS_VGET(");
    const size_t unbusyPos = vgetReg.find("vfs_unbusy(busy_mp);");
    const size_t metaPos = vgetReg.find("switch_prefetch_vnode_meta(");
    EXPECT_NE(busyPos, std::string::npos);
    EXPECT_NE(vgetPos, std::string::npos);
    EXPECT_NE(unbusyPos, std::string::npos);
    EXPECT_NE(metaPos, std::string::npos);
    if (busyPos != std::string::npos && vgetPos != std::string::npos && unbusyPos != std::string::npos &&
        metaPos != std::string::npos) {
        EXPECT_LT(busyPos, vgetPos);
        EXPECT_LT(vgetPos, unbusyPos);
        EXPECT_LT(unbusyPos, metaPos);
    }

    const std::string saveNext = bodyOf(prefetchSave, "switch_prefetch_save_next(");
    EXPECT_THAT(saveNext, HasSubstr("switch_prefetch_busy_mount(mounts[v], fsids[v])"));
    EXPECT_THAT(saveNext, HasSubstr("vfs_unbusy(busy_mp);"));
    EXPECT_THAT(bodyOf(prefetchSave, "collect_kept_replayed("), HasSubstr("switch_prefetch_bit_test("));
    EXPECT_THAT(bodyOf(prefetchSave, "collect_kept_replayed("), HasSubstr("switch_prefetch_vget_regular("));
}
