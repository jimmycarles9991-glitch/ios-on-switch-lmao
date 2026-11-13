// SPDX-License-Identifier: MIT
// predicates.tsv names every refusal predicate RawMetalPredicates.h has, once, and each fate points at a rule, a validation
// problem or a layer that exists. The header imports Foundation, so its name table is read as text.
#include "../plan_validate.h"
#include "bazel/testing/source_text.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace raw::plan {
namespace {

std::vector<std::string> predicateNames()
{
    const std::string header = switch_ios::test::SourceText("../../runtime/RawMetalPredicates.h");
    const size_t at = header.find("kPredNames[] = {");
    const size_t end = header.find("};", at);
    std::vector<std::string> names;
    for (size_t q = header.find('"', at); q != std::string::npos && q < end; q = header.find('"', q + 1)) {
        const size_t close = header.find('"', q + 1);
        names.push_back(header.substr(q + 1, close - q - 1));
        q = close;
    }
    return names;
}

struct Row {
    std::string tag, fires, fate, covered, note;
};

std::vector<Row> table()
{
    std::istringstream in(switch_ios::test::SourceText("../predicates.tsv"));
    std::vector<Row> rows;
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream fields(line);
        Row r;
        std::getline(fields, r.tag, '\t');
        std::getline(fields, r.fires, '\t');
        std::getline(fields, r.fate, '\t');
        std::getline(fields, r.covered, '\t');
        std::getline(fields, r.note, '\t');
        rows.push_back(r);
    }
    return rows;
}

std::vector<std::string> split(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string item; std::getline(in, item, ',');)
        out.push_back(item);
    return out;
}

template <size_t N> bool named(const char *const (&names)[N], const std::string &name)
{
    return std::find_if(std::begin(names), std::end(names), [&](const char *n) {
               return name == n;
           }) != std::end(names);
}

TEST(Predicates, EveryPredicateOnce)
{
    std::vector<std::string> names = predicateNames();
    ASSERT_EQ(names.size(), 62u) << "the enum's 61 predicates and PredNone's '-'";
    names.erase(names.begin());
    std::set<std::string> tags;
    for (const Row &r : table())
        EXPECT_TRUE(tags.insert(r.tag).second) << r.tag << " twice";
    EXPECT_EQ(tags, std::set<std::string>(names.begin(), names.end()));
}

TEST(Predicates, FatesPointAtWhatExists)
{
    const std::set<std::string> fates = {"rule", "client", "query", "cb-error", "gap", "layer", "nil", "m2v", "growth"};
    for (const Row &r : table()) {
        EXPECT_TRUE(fates.count(r.fate)) << r.tag << ": " << r.fate;
        EXPECT_TRUE(r.fires == "create" || r.fires == "record" || r.fires == "encode") << r.tag;
        if (r.covered == "-") {
            EXPECT_TRUE(r.fate != "rule" && r.fate != "client" && r.fate != "query" && r.fate != "layer") << r.tag;
            continue;
        }
        for (const std::string &item : split(r.covered)) {
            if (r.fate == "rule")
                EXPECT_TRUE(named(kRuleNames, item)) << r.tag << ": no rule " << item;
            else if (r.fate == "layer")
                EXPECT_TRUE(item == "L3" || item == "L4" || item == "L6") << r.tag << ": " << item;
            else
                EXPECT_TRUE(named(kProblemNames, item)) << r.tag << ": no problem " << item;
        }
    }
}

}  // namespace
}  // namespace raw::plan
