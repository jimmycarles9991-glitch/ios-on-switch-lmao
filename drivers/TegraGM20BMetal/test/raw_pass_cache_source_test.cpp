// SPDX-License-Identifier: MIT
// Pass lookup and insertion have no synchronization; the recording pool transfers their bank's ownership.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

namespace {

const std::string &cache()
{
    static const std::string source = SourceText("../raw/runtime/RawMetalPassCache.mm");
    return source;
}

TEST(RawPassCacheSource, ThePassPathTakesNoLockAndNoReadModifyWrite)
{
    for (const char *head : {"RMPassEntry *rmPassFind(", "void rmPassKeep(", "void dropIdle(RMPassCache &c)",
                             "CFTypeRef take(RMPassCache &c, size_t i)"}) {
        const CodeText body = Block(cache(), head);
        for (const char *banned : {"_lock", "lock_guard", "mutex", "fetch_", "exchange", "__atomic", "OSAtomic",
                                   "@synchronized", "memory_order", ".load(", ".store("})
            EXPECT_FALSE(body.has(banned)) << head << " " << banned;
    }
}

}  // namespace
