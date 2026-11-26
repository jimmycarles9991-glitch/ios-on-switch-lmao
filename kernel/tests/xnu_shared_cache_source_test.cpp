// A DriverKit process maps its own dyld shared cache, which only the OS cryptex has. iOS 17's dyld opens it at
// /System/DriverKit/System/Library/dyld alone, where the boot shows the cryptex's directory through a bindfs union, so
// xnu-10063's volume check judges a boot union's vnode by the file behind it. iOS 27's dyld goes on to the cryptex's own
// path when that one is missing, so 27 has no such union and xnu-12377's check stays as it was; xnu-12377 maps a
// DriverKit cache only when it was translated offline.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::Configured;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
constexpr const char *kDriverKitUnion = "\"/System/DriverKit/System/Library\",";

std::string setup(const char *tree)
{
    return Body(SourceText(std::string("../") + tree + "/bsd/vm/vm_unix.c"), "\nshared_region_map_and_slide_setup(\n");
}
}  // namespace

TEST(XnuSharedCacheSource, Xnu10063JudgesABootUnionByItsFile)
{
    const std::string body = setup("xnu-10063.141.1");
    EXPECT_THAT(body,
                HasSubstr("switch_cryptex = switch_shared_cache_on_cryptex(p, srfmp->vp, cryptex_vp->v_mount) &&"));
    EXPECT_THAT(body, Not(HasSubstr("cryptex_vp->v_mount == srfmp->vp->v_mount")));
    const std::string helper = Body(SourceText("../switch/vm/shared_cache.c"), "bool switch_shared_cache_on_cryptex(");
    EXPECT_THAT(helper, HasSubstr("bindfs_boot_union_host(mp) == NULL"));
    EXPECT_THAT(helper, HasSubstr("vnode_put(file);"));
}

TEST(XnuSharedCacheSource, Xnu12377MapsOnlyATranslatedDriverKitCache)
{
    const std::string body = setup("xnu-12377.121.6");
    EXPECT_THAT(body, HasSubstr("switch_shared_cache_unusable(p, srfmp->vp, is_driverkit)"));
    EXPECT_THAT(body, HasSubstr("switch_cryptex = cryptex_vp->v_mount == srfmp->vp->v_mount &&"));
    EXPECT_THAT(body, Not(HasSubstr("switch_shared_cache_on_cryptex")));
}

TEST(XnuSharedCacheSource, OnlyIos17UnionsTheDriverKitCache)
{
    const std::string mount = SourceText("../../diagnostics/switch/SwitchRawMount.h");
    EXPECT_THAT(Configured(mount, "XNU_12377_121_6", false), HasSubstr(kDriverKitUnion));
    EXPECT_THAT(Configured(mount, "XNU_12377_121_6", true), Not(HasSubstr(kDriverKitUnion)));
}
