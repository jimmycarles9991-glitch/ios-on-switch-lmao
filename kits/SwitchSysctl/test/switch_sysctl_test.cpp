// switch_sysctl_oid fills what arm64 sysctl_register_oid requires: CTLFLAG_OID2 and SYSCTL_OID_VERSION.
#include "kits/SwitchSysctl/SwitchSysctl.h"

#include <gtest/gtest.h>

namespace {
int handler(struct sysctl_oid *, void *, int, struct sysctl_req *)
{
    return 0;
}

TEST(SwitchSysctl, OidIsVersioned)
{
    struct sysctl_oid_list parent = {};
    struct sysctl_oid oid;
    memset(&oid, 0x5a, sizeof(oid));
    switch_sysctl_oid(&oid, &parent, "monotonicclock", CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_KERN, handler, "IU", "d");
    EXPECT_EQ(oid.oid_kind, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_OID2);
    EXPECT_EQ(oid.oid_version, SYSCTL_OID_VERSION);
    EXPECT_EQ(oid.oid_number, OID_AUTO);
    EXPECT_EQ(oid.oid_parent, &parent);
    EXPECT_EQ(oid.oid_link.sle_next, nullptr);
    EXPECT_EQ(oid.oid_arg1, nullptr);
    EXPECT_EQ(oid.oid_refcnt, 0);
}
}  // namespace
