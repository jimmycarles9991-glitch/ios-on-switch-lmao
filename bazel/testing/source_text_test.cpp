// CodeText, Block and Occurrences give the same answers for one piece of code however it is laid out.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::Occurrences;

const std::string kNarrow = R"(// a comment's apostrophe, and a brace: {
static int decide(int a,
                  int b);

static int decide(int a,
                  int b)
{
    if (a > b) {
        log("a won by "
            "%d", a - b);
        return a;
    }
    const char *brace = "}";
    return b;
}
)";

const std::string kWide = R"(// a comment's apostrophe, and a brace: {
static int decide(int a, int b);
static int decide(int a, int b) {
	if (a>b)
	{
		log("a won by %d", a - b);
		return a;
	}
	const char* brace = "}";
	return b;
}
)";

TEST(CodeText, ALayoutChangesNoAnswer)
{
    for (const std::string &source : {kNarrow, kWide}) {
        const CodeText code(source);
        EXPECT_TRUE(code.Has("static int decide(int a, int b) {"));
        EXPECT_TRUE(code.Has("log(\"a won by %d\", a - b);"));
        EXPECT_TRUE(code.Has("won by %d"));
        EXPECT_FALSE(code.Has("return c;"));
        EXPECT_EQ(code.Count("return"), 2u);
        EXPECT_TRUE(code.Before("return a;", "return b;"));
        EXPECT_FALSE(code.Before("return b;", "return a;"));
        EXPECT_FALSE(code.Before("return a;", "return c;"));
        EXPECT_TRUE(code.Between("if (a > b)", "return a;").Has("log("));
        EXPECT_TRUE(code.After("const char *brace").StartsWith("= \"}\";"));
        EXPECT_TRUE(code.AfterLast("return").StartsWith("b;"));
        EXPECT_TRUE(code.EndsWith("return b; }"));
    }
}

TEST(CodeText, BlockMatchesBracesInCodeOnlyAndSkipsDeclarations)
{
    for (const std::string &source : {kNarrow, kWide}) {
        const CodeText body = Block(source, "static int decide(int a, int b)");
        EXPECT_TRUE(body.StartsWith("if (a > b) {"));
        EXPECT_TRUE(body.EndsWith("const char *brace = \"}\"; return b;"));
        EXPECT_TRUE(Block(source, "if (a > b)").EndsWith("return a;"));
    }
}

TEST(CodeText, ABlockIsNotADefaultArgument)
{
    const std::string source =
        "void place(int at, const Shape &shape = {},\n           int turns = 0)\n{\n    put(at);\n}\n";
    EXPECT_TRUE(Block(source, "void place(int at,").StartsWith("put(at);"));
}

TEST(CodeText, OccurrencesNameTheirFunction)
{
    for (const std::string &source : {kNarrow, kWide}) {
        const auto found = Occurrences(source, "return");
        ASSERT_EQ(found.size(), 2u);
        EXPECT_TRUE(found[0].head.EndsWith("static int decide(int a, int b)"));
        EXPECT_FALSE(found[0].head.Has("int b);"));
        EXPECT_TRUE(found[0].before.EndsWith("a - b);"));
        EXPECT_TRUE(found[1].before.Has("const char *brace = \"}\";"));
    }
}

}  // namespace
