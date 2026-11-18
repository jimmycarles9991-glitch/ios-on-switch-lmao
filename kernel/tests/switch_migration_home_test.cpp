// SPDX-License-Identifier: GPL-2.0-only
#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>

namespace {
using proc_t = void *;
unsigned uid;
bool helper;

struct Request {
    int *newptr;
    int value;
};
#define SYSCTL_HANDLER_ARGS (Request *req, int oidp = 0, int arg1 = 0, int arg2 = 0)
#define SYSCTL_OUT(req, src, len) (std::memcpy(&(req)->value, (src), (len)), 0)
#define SYSCTL_IN(req, value, len) (std::memcpy((value), (req)->newptr, (len)), 0)
#define SYSCTL_PROC(...)
#define os_atomic_load(p, order) (*(p))
#define os_atomic_store(p, value, order) (*(p) = (value))

proc_t current_proc() { return nullptr; }
bool switch_bootstrap_service_caller(proc_t) { return helper; }
unsigned kauth_cred_get() { return uid; }
unsigned kauth_cred_getuid(unsigned value) { return value; }

#include "migration_home_impl.h"

class MigrationWindow : public testing::Test {
protected:
    void SetUp() override
    {
        switch_migration_scheduled = false;
        switch_home_native = false;
        uid = 0;
        helper = true;
    }
    static int ready()
    {
        Request req{};
        EXPECT_EQ(switch_home_ready_sysctl(&req), 0);
        return req.value;
    }
    static int release(int value = 1)
    {
        Request req{&value, -1};
        return switch_home_ready_sysctl(&req);
    }
};

TEST_F(MigrationWindow, OnlyPackagedFreshOrPendingMigrationOpensAWindow)
{
    switch_migration_home_prepare(true, false);
    EXPECT_EQ(ready(), 1);
    switch_migration_home_requested();
    switch_migration_home_prepare(false, true);
    EXPECT_EQ(ready(), 1);
    switch_migration_home_prepare(true, false);
    EXPECT_EQ(ready(), 0);
}

TEST_F(MigrationWindow, OnlyRootHelperCanCloseTheWindow)
{
    switch_migration_home_prepare(true, true);
    helper = false;
    EXPECT_EQ(release(), EPERM);
    uid = 501;
    helper = true;
    EXPECT_EQ(release(), EPERM);
    uid = 0;
    EXPECT_EQ(release(0), EINVAL);
    EXPECT_EQ(ready(), 0);
    EXPECT_EQ(release(), 0);
    EXPECT_EQ(release(0), EINVAL);
    EXPECT_EQ(ready(), 1);
}

TEST_F(MigrationWindow, PackageOnlyUpdateStaysScheduledUntilTheWorkerFinishes)
{
    switch_migration_home_prepare(true, true);
    EXPECT_EQ(ready(), 0);
    EXPECT_EQ(release(), 0);
    EXPECT_EQ(ready(), 1);
}
} // namespace
