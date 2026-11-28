// xnu-12377's shared region pager fills the pinned dyld function-variant slots (switch/fvariant.h) after both slide
// paths, including pages with no rebases; each region looks its cache up once and every slide info carries the result;
// xnu-10063 is untouched.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::SourceText;

namespace {

size_t Find(const std::string &text, const std::string &what, size_t from = 0)
{
    const size_t at = text.find(what, from);
    EXPECT_NE(at, std::string::npos) << what;
    return at;
}

TEST(XnuFvariantSource, PagerFillsAfterEachSlidePath)
{
    const std::string pager = SourceText("../xnu-12377.121.6/osfmk/vm/vm_shared_region_pager.c");
    const std::string request = Between(pager, "\nshared_region_pager_data_request(", "\n}\n");

    const size_t part = Find(request, "shared_region_pager_slide_part(pager, dst_vaddr");
    const size_t partOk = Find(request, "shared_region_pager_slid++;", part);
    const size_t partFill = Find(request, "switch_fvariant_fill_page(", partOk);
    EXPECT_LT(partFill, Find(request, "goto part_done;", part));

    const size_t page = Find(request, "kr = vm_shared_region_slide_page(pager->srp_slide_info");
    const size_t pageOk = Find(request, "shared_region_pager_slid++;", page);
    const size_t pageFill = Find(request, "switch_fvariant_fill_page(pager->srp_slide_info", pageOk);
    EXPECT_NE(request.find("offset_in_sliding_range + slide_start_address, slide_info_page_size", pageFill),
              std::string::npos);
    // The fill runs in the pager, after vm_shared_region_slide_page() returned: its v5 NO_REBASE early return cannot
    // skip it.
    EXPECT_EQ(Between(pager, "\nshared_region_pager_slide_part(", "\n}\n").find("switch_fvariant"), std::string::npos);
}

TEST(XnuFvariantSource, EachRegionLooksItsCacheUpOnceBeforeItsMappingsSlide)
{
    const std::string region = SourceText("../xnu-12377.121.6/osfmk/vm/vm_shared_region.c");
    const std::string map = Between(region, "\nvm_shared_region_map_file(\n", "\n}\n");
    const size_t switched = Find(map, "switch_ctx = vm_map_switch_to(shared_region->sr_config_map);");
    const std::string call = "shared_region->sr_switch_fvariant = switch_fvariant_for_region(shared_region);";
    const size_t lookup = Find(map, call);
    EXPECT_LT(switched, lookup);
    EXPECT_LT(lookup, Find(map, "kr = vm_shared_region_slide(shared_region->sr_slide,"));
    EXPECT_EQ(map.find("switch_fvariant_for_region", lookup + call.size()), std::string::npos) << "once per region";
    EXPECT_NE(
        Between(region, "\nvm_shared_region_create(\n", "\n}\n").find("shared_region->sr_switch_fvariant = NULL;"),
        std::string::npos);

    const std::string slide = Between(region, "\nvm_shared_region_slide_mapping(\n", "\n}\n");
    const size_t alloc = Find(slide, "si = kalloc_type(struct vm_shared_region_slide_info");
    const size_t set = Find(slide, "si->si_switch_fvariant = sr->sr_switch_fvariant;", alloc);
    EXPECT_LT(set, Find(slide, "#if __has_feature(ptrauth_calls)", alloc));

    const std::string header = SourceText("../xnu-12377.121.6/osfmk/vm/vm_shared_region_xnu.h");
    EXPECT_NE(Between(header, "typedef struct vm_shared_region_slide_info {", "} *vm_shared_region_slide_info_t;")
                  .find("const struct switch_fvariant_cache *si_switch_fvariant;"),
              std::string::npos);
    EXPECT_NE(Between(header, "struct vm_shared_region {", "\n};")
                  .find("const struct switch_fvariant_cache *sr_switch_fvariant;"),
              std::string::npos);
}

TEST(XnuFvariantSource, TheLookupReadsTheMainHeaderAndClassifiesIt)
{
    // The UUID is read where vm_shared_region_map_file_final() reads the header, and the bytes go to the classifier
    // switch_fvariant_test checks against the shipped headers.
    const std::string glue = SourceText("../switch/vm/fvariant.c");
    const std::string lookup = Between(glue, "switch_fvariant_for_region(struct vm_shared_region *sr)\n{", "\n}\n");
    const size_t read =
        Find(lookup, "copyin((user_addr_t)(sr->sr_base_address + sr->sr_first_mapping), header, sizeof(header))");
    const size_t classify = Find(lookup, "switch_fvariant_classify(header, SWITCH_FVARIANT_CACHES,", read);
    EXPECT_LT(read, classify);
    // What it returns is what the classifier picked: NULL only for a cache with no pinned slots.
    EXPECT_NE(lookup.find("&cache)) {", classify), std::string::npos);
    EXPECT_NE(lookup.find("\n    return cache;", classify), std::string::npos);
    // The one NULL return is the header read's failure branch, which logs once.
    const size_t failure = Find(lookup, ") != 0) {", read);
    const size_t null = Find(lookup, "return NULL;");
    EXPECT_EQ(lookup.find("return NULL;", null + 1), std::string::npos);
    EXPECT_LT(failure, null);
    EXPECT_LT(null, classify);
    EXPECT_NE(lookup.substr(failure, null - failure).find("os_log_error("), std::string::npos);
    EXPECT_EQ(lookup.find("cache = NULL", lookup.find("cache = NULL") + 1), std::string::npos)
        << "only its declaration";
    EXPECT_NE(lookup.find("os_log_error(", classify), std::string::npos) << "an unpinned cache logs in release too";

    const std::string region = SourceText("../xnu-12377.121.6/osfmk/vm/vm_shared_region.c");
    EXPECT_EQ(
        Between(region, "#include <vm/vm_shared_region.h>", "#include <vm/vm_protos.h>").find("<switch/fvariant.h>"),
        std::string::npos);
    EXPECT_NE(region.find("#include <switch/fvariant_hooks.h>"), std::string::npos);
    const std::string pager = SourceText("../xnu-12377.121.6/osfmk/vm/vm_shared_region_pager.c");
    EXPECT_NE(pager.find("#include <switch/fvariant_hooks.h>"), std::string::npos);
    EXPECT_EQ(pager.find("#include <switch/fvariant.h>"), std::string::npos);
}

TEST(XnuFvariantSource, OnlyTheIos27TreeHasIt)
{
    for (const char *file :
         {"../xnu-10063.141.1/osfmk/vm/vm_shared_region_pager.c", "../xnu-10063.141.1/osfmk/vm/vm_shared_region.c"}) {
        EXPECT_EQ(SourceText(file).find("switch_fvariant"), std::string::npos) << file;
    }
    // Built for 12377 only, with the kind's header so that switch/fvariant.h picks the kind's table or fails.
    const std::string objects = SourceText("../switch/objects.bzl");
    EXPECT_NE(
        objects.find(
            R"(("osfmk/vm/vm_shared_region.c", "vm/fvariant.c", ["-include", "{owner_header}"], ["12377.121.6"]))"),
        std::string::npos);
}

}  // namespace
