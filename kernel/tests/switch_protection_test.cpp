// <switch/protection_rules.h>: which class an unlabelled vnode reads as, the label a new one needs, the label kept out
// of listxattr; and where 12377's hooks sit: after authorization and the MAC check, before the filesystem sees a
// class, with every failed label write failing its call.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <switch/protection_rules.h>

using testing::HasSubstr;
using testing::Not;

namespace {

std::string Text(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string Body(const std::string &source, const std::string &signature)
{
    const size_t start = source.find(signature);
    EXPECT_NE(start, std::string::npos) << signature;
    return source.substr(start, source.find("\n}\n", start) - start);
}

size_t Hide(std::string *names)
{
    return switch_protection_hide(names->data(), names->size(), SWITCH_PROTECTION_XATTR);
}

const std::string kLabel = std::string(SWITCH_PROTECTION_XATTR) + '\0';

TEST(SwitchProtection, UnlabelledVnodesBeforeTheEpochReadAsD)
{
    EXPECT_EQ(switch_protection_unlabelled(false, true, 99, 100), SWITCH_PROTECTION_D);
    EXPECT_EQ(switch_protection_unlabelled(true, true, 99, 100), SWITCH_PROTECTION_D);
}

TEST(SwitchProtection, UnlabelledVnodesSinceTheEpochReadAsNewOnes)
{
    EXPECT_EQ(switch_protection_unlabelled(false, true, 100, 100), SWITCH_PROTECTION_C);
    EXPECT_EQ(switch_protection_unlabelled(true, true, 101, 100), SWITCH_PROTECTION_DIR_NONE);
    EXPECT_EQ(switch_protection_unlabelled(false, false, 0, 100), SWITCH_PROTECTION_C);
}

TEST(SwitchProtection, ANewVnodeIsLabelledOnlyWhenItWouldReadOtherwise)
{
    EXPECT_EQ(switch_protection_created_label(false, SWITCH_PROTECTION_DEFAULT, false, 0), SWITCH_PROTECTION_DEFAULT);
    EXPECT_EQ(switch_protection_created_label(false, SWITCH_PROTECTION_C, false, 0), SWITCH_PROTECTION_DEFAULT);
    EXPECT_EQ(switch_protection_created_label(false, SWITCH_PROTECTION_D, false, 0), SWITCH_PROTECTION_D);
    EXPECT_EQ(switch_protection_created_label(false, 1, true, SWITCH_PROTECTION_D), 1);
    EXPECT_EQ(switch_protection_created_label(true, SWITCH_PROTECTION_DIR_NONE, false, 0), SWITCH_PROTECTION_DEFAULT);
}

TEST(SwitchProtection, ANewVnodeTakesItsLabelledDirectorysClass)
{
    EXPECT_EQ(switch_protection_created_label(false, SWITCH_PROTECTION_DEFAULT, true, SWITCH_PROTECTION_D),
              SWITCH_PROTECTION_D);
    EXPECT_EQ(switch_protection_created_label(true, SWITCH_PROTECTION_DEFAULT, true, SWITCH_PROTECTION_C),
              SWITCH_PROTECTION_C);
    EXPECT_EQ(switch_protection_created_label(false, SWITCH_PROTECTION_DEFAULT, true, SWITCH_PROTECTION_C),
              SWITCH_PROTECTION_DEFAULT);
    EXPECT_EQ(switch_protection_created_label(false, SWITCH_PROTECTION_DEFAULT, true, SWITCH_PROTECTION_DIR_NONE),
              SWITCH_PROTECTION_DEFAULT);
    EXPECT_EQ(switch_protection_created_label(true, SWITCH_PROTECTION_DEFAULT, true, SWITCH_PROTECTION_DIR_NONE),
              SWITCH_PROTECTION_DEFAULT);
}

TEST(SwitchProtection, ListxattrLosesOnlyTheLabel)
{
    std::string names = std::string("a\0", 2) + kLabel + std::string("b\0", 2);
    EXPECT_EQ(Hide(&names), 4u);
    EXPECT_EQ(names.substr(0, 4), std::string("a\0b\0", 4));

    names = kLabel + std::string("b\0", 2);
    EXPECT_EQ(Hide(&names), 2u);
    EXPECT_EQ(names.substr(0, 2), std::string("b\0", 2));

    names = kLabel;
    EXPECT_EQ(Hide(&names), 0u);

    names = std::string(SWITCH_PROTECTION_XATTR) + "X" + '\0' + "c";
    EXPECT_EQ(Hide(&names), names.size());
}

TEST(SwitchProtection, TheLabelIsInTheNamespaceUserlandCannotTouch)
{
    EXPECT_EQ(strncmp(SWITCH_PROTECTION_XATTR, "com.apple.system.", 17), 0);
    const std::string xattr = Text(VFS_XATTR_12377);
    EXPECT_THAT(Body(xattr, "\nxattr_protected(const char *attrname)\n"),
                HasSubstr("return !strncmp(attrname, \"com.apple.system.\", 17);"));
    const std::string check = Body(Text(VFS_SYSCALLS_12377), "\nxattr_entitlement_check(");
    EXPECT_THAT(check, HasSubstr("if (strcmp(attrname, KAUTH_FILESEC_XATTR) == 0) {"));
    EXPECT_THAT(check, HasSubstr("return EPERM;"));
}

TEST(SwitchProtection, SetattrlistLabelsAfterAuthorizationAndTheMacCheck)
{
    const std::string body = Body(Text(VFS_ATTRLIST_12377), "\nsetattrlist_internal(vnode_t vp,");
    const size_t unpack =
        body.find("ATTR_UNPACK(va.va_dataprotect_class);\n"
                  "\t\tVATTR_SET_ACTIVE(&va, va_dataprotect_class);\n"
                  "#if CONFIG_MACF\n"
                  "\t\tif ((error = mac_vnode_check_dataprotect_set(ctx, vp, &va.va_dataprotect_class)))");
    const size_t authorize = body.find("vnode_authorize(vp, NULL, action, ctx)");
    const size_t begin = body.find("switch_protection_setattr_begin(vp, &va, &switch_class, ctx)");
    const size_t setattr = body.find("vnode_setattr(vp, &va, ctx)");
    const size_t end = body.find("if ((error = switch_protection_setattr_end(vp, switch_class, ctx)) != 0) {");
    ASSERT_NE(unpack, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    EXPECT_LT(unpack, authorize);
    EXPECT_LT(authorize, begin);
    EXPECT_LT(begin, setattr);
    EXPECT_LT(setattr, end);
    EXPECT_THAT(body, Not(HasSubstr("switch_protection_set(")));
}

TEST(SwitchProtection, FcntlLabelsAfterTheWriteRightAndTheMacCheck)
{
    const std::string source = Text(KERN_DESCRIP_12377);
    const size_t set = source.find("\tcase F_SETPROTECTIONCLASS: {");
    ASSERT_NE(set, std::string::npos);
    const std::string body = source.substr(set, source.find("\tcase F_TRANSCODEKEY: {", set) - set);
    const size_t right = body.find("(KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA)");
    const size_t mac = body.find("mac_vnode_check_dataprotect_set(ctx, vp, &dataprotect_class)");
    const size_t label = body.find("error = switch_protection_set(vp, (int32_t)tmp, ctx);");
    ASSERT_NE(label, std::string::npos);
    EXPECT_LT(right, mac);
    EXPECT_LT(mac, label);
}

TEST(SwitchProtection, ACreateWhoseLabelFailsFails)
{
    const std::string body = Body(Text(VFS_SUBR_12377), "\nvn_create(vnode_t dvp,");
    const size_t begin = body.find("if ((error = switch_protection_create_begin(dvp, vap, &switch_class)) != 0) {");
    const size_t create = body.find("error = vn_create_reg(");
    const size_t end = body.find("error = switch_protection_create_end(dvp, *vpp, switch_class, ctx);");
    const size_t cleanup = body.find("if ((error != 0) && (vp != (vnode_t)0)) {");
    ASSERT_NE(end, std::string::npos);
    EXPECT_LT(begin, create);
    EXPECT_LT(create, end);
    EXPECT_LT(end, cleanup);
}

TEST(SwitchProtection, BulkClassRequestsOnDataGoThroughGetattrlist)
{
    const std::string body = Body(Text(VFS_ATTRLIST_12377), "\ngetattrlistbulk(proc_t p,");
    const size_t fallback =
        body.find("|| switch_protection_bulk_by_entry(dvp, al.commonattr)) {\n\t\terror = ENOTSUP;");
    EXPECT_NE(fallback, std::string::npos);
    EXPECT_LT(fallback, body.find("VNOP_GETATTRLISTBULK(dvp,"));
}

TEST(SwitchProtection, ListxattrAndStatfsGoThroughTheHooks)
{
    const std::string syscalls = Text(VFS_SYSCALLS_12377);
    for (const char *signature : {"\nlistxattr(proc_t p,", "\nflistxattr(proc_t p,"}) {
        const std::string body = Body(syscalls, signature);
        EXPECT_THAT(body, HasSubstr("error = switch_protection_listxattr(vp, auio, &attrsize, uap->options,"))
            << signature;
        EXPECT_THAT(body, Not(HasSubstr("vn_listxattr("))) << signature;
    }
    const std::string flags = "f_flags = (mp->mnt_flag & MNT_VISFLAGMASK) | switch_protection_mount_flags(mp);";
    for (const char *path : {VFS_SYSCALLS_12377, VFS_SUBR_12377}) {
        const std::string source = Text(path);
        EXPECT_THAT(source, Not(HasSubstr("f_flags = mp->mnt_flag & MNT_VISFLAGMASK;"))) << path;
        EXPECT_THAT(source, HasSubstr(flags)) << path;
    }
}

}  // namespace
