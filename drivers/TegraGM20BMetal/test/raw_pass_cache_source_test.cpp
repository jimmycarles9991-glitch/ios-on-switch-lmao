// SPDX-License-Identifier: MIT
// The pass cache's path on the encoding thread (RawMetalPassCache.mm rmPassFind, rmPassDone, rmPassKeep and what they
// call) takes no lock and makes no atomic read-modify-write: only rmPassTrim and the thread's first and last use touch
// the registry's mutex, and rmPassTrim takes a thread's copies only with that thread stopped.
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
    for (const char *head :
         {"RMPassEntry *rmPassFind(", "void rmPassDone()", "void rmPassKeep(", "void enter(Cache &c)",
          "void leave(Cache &c)", "void dropIdle(Cache &c,", "CFTypeRef take(Cache &c, size_t i)",
          "void count(Cache &c, int64_t passes, int64_t textures)"}) {
        const CodeText body = Block(cache(), head);
        for (const char *banned :
             {"_lock", "lock_guard", "mutex", "fetch_", "exchange", "__atomic", "OSAtomic", "@synchronized"})
            EXPECT_FALSE(body.Has(banned)) << head << " " << banned;
    }
}

TEST(RawPassCacheSource, TrimTakesCopiesOnlyFromAStoppedThreadOutsideItsWindow)
{
    const CodeText trim = Block(cache(), "RMPassTrim rmPassTrim()");
    EXPECT_TRUE(trim.Before("thread_suspend(c->thread)", "c->inside.load(std::memory_order_relaxed)"));
    EXPECT_TRUE(trim.Before("gone[i] = take(*c, i);", "thread_resume(c->thread);"));
    EXPECT_TRUE(trim.Before("thread_resume(c->thread);", "release(gone);"));
    EXPECT_TRUE(trim.Has("if (other && thread_suspend(c->thread) != KERN_SUCCESS) { "
                         "c->dropAsked.store(true, std::memory_order_relaxed); continue; }"))
        << "a thread trim cannot stop keeps its copies only until its next lookup";
}

}  // namespace
