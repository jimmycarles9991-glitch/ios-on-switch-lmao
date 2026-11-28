// kernel/switch/diag/deployed.h on the host: the fields of the deployed.json tools/deploy/deployed.py writes.
#include <gtest/gtest.h>

#include <string>

#include "diag/deployed.h"

namespace {

TEST(XnuDiagDeployed, ReadsWhatTheDeployWrites)
{
    // json.dumps(info, indent=1), as deployed.record writes it.
    const char *json = "{\n \"commit\": \"64a91a41a0123456789abcdef0123456789abcde\",\n \"branch\": \"integrate/"
                       "overnight-1\",\n \"dirty\": true,\n \"build\": \"24B119\",\n \"deployed_at\": "
                       "\"2026-11-25T19:08:11+00:00\"\n}\n";
    switch_deployed d = {};
    switch_deployed_parse(json, &d);
    EXPECT_EQ(std::string(d.commit), "64a91a41a0123456789abcdef0123456789abcde");
    EXPECT_EQ(std::string(d.branch), "integrate/overnight-1");
    EXPECT_EQ(std::string(d.build), "24B119");
    EXPECT_EQ(std::string(d.deployed_at), "2026-11-25T19:08:11+00:00");
    EXPECT_TRUE(d.dirty);
}

TEST(XnuDiagDeployed, AMissingOrOverlongFieldReadsAsUnknown)
{
    switch_deployed d = {};
    const std::string long_branch =
        "{\"commit\": \"abc\", \"dirty\": false, \"branch\": \"" + std::string(80, 'b') + "\"}";
    switch_deployed_parse(long_branch.c_str(), &d);
    EXPECT_EQ(std::string(d.commit), "abc");
    EXPECT_FALSE(d.dirty);
    EXPECT_EQ(std::string(d.branch), "?");
    EXPECT_EQ(std::string(d.build), "?");
    switch_deployed_parse("{\"commit\": \"cut", &d);
    EXPECT_EQ(std::string(d.commit), "?") << "a value with no closing quote";
}

}  // namespace
