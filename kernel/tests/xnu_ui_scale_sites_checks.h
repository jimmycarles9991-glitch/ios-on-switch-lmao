// What every IPSW kind's UI scale sites (bsd/vm/switch_ui_scale_sites_<build>.h) must hold against that kind's cache
// text mappings, for the per-kind site tests.
#ifndef SWITCH_XNU_UI_SCALE_SITES_CHECKS_H
#define SWITCH_XNU_UI_SCALE_SITES_CHECKS_H

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

extern "C" {
#include "vm/switch_ui_scale.h"
#include "vm/switch_ui_scale_sites.h"
}

namespace switch_ui_scale_sites {

// A subcache's text mapping as `ipsw dyld info` gives it: __TEXT is each subcache's only executable mapping, at file
// offset 0. other_pagers_slots: the slots of this mapping another cache text pager takes.
struct TextMapping {
    const char *name;
    uint8_t uuid[16];
    uint64_t address, size;
    unsigned other_pagers_slots;
};

constexpr uint64_t kChunk = 0x1000;

inline std::string NameOf(const switch_us_site &site)
{
    return std::string(site.name) + " " + site.applied;
}

inline const switch_us_site *Named(const switch_us_cache &cache, const std::string &name)
{
    for (size_t i = 0; i < cache.count; i++) {
        if (NameOf(cache.sites[i]) == name) {
            return &cache.sites[i];
        }
    }
    ADD_FAILURE() << "no site " << name << " in " << cache.build;
    return nullptr;
}

inline uint32_t Word(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8 |
           static_cast<uint32_t>(bytes[2]) << 16 | static_cast<uint32_t>(bytes[3]) << 24;
}

// A branch's target from its address: b's and bl's imm26, b.cond/cbz/cbnz's imm19 or tbz/tbnz's imm14, in words.
inline uint64_t BranchTarget(uint64_t address, uint32_t insn)
{
    if ((insn & 0x7c000000u) == 0x14000000u) {
        const int64_t imm26 = static_cast<int64_t>(insn & 0x3ffffffu);
        return address + static_cast<uint64_t>((imm26 >= 0x2000000 ? imm26 - 0x4000000 : imm26) * 4);
    }
    if ((insn & 0xff000010u) == 0x54000000u || (insn & 0x7e000000u) == 0x34000000u) {
        const int64_t imm19 = static_cast<int64_t>((insn >> 5) & 0x7ffff);
        return address + static_cast<uint64_t>((imm19 >= 0x40000 ? imm19 - 0x80000 : imm19) * 4);
    }
    const int64_t imm14 = static_cast<int64_t>((insn >> 5) & 0x3fff);
    return address + static_cast<uint64_t>((imm14 >= 0x2000 ? imm14 - 0x4000 : imm14) * 4);
}

// The unslid address of an edit's byte `at`.
inline uint64_t AddressOf(const switch_us_site &site, const switch_us_edit &edit, unsigned at)
{
    return site.page_vmaddr + (edit.fileoff - site.page_fileoff) + at;
}

inline unsigned TextSlots(const char *vm_shared_region)
{
    const std::string source = switch_ios::test::SourceText(vm_shared_region);
    const std::string define = "#define SWITCH_TEXT_SLOTS ";
    const size_t at = source.find(define);
    if (at == std::string::npos) {
        ADD_FAILURE() << "no SWITCH_TEXT_SLOTS in " << vm_shared_region;
        return 0;
    }
    return static_cast<unsigned>(std::stoul(source.substr(at + define.size())));
}

inline const TextMapping *MappingOf(const std::vector<TextMapping> &mappings, const switch_us_site &site)
{
    for (const TextMapping &mapping : mappings) {
        if (std::memcmp(mapping.uuid, site.uuid, sizeof(site.uuid)) == 0) {
            return &mapping;
        }
    }
    return nullptr;
}

inline void ExpectEveryEditInItsSitesChunk(const switch_us_cache &cache, const std::vector<TextMapping> &mappings)
{
    EXPECT_LE(cache.count, SWITCH_US_MAX_SITES);
    for (size_t s = 0; s < cache.count; s++) {
        const switch_us_site &site = cache.sites[s];
        SCOPED_TRACE(NameOf(site));
        const TextMapping *mapping = MappingOf(mappings, site);
        ASSERT_NE(mapping, nullptr);
        EXPECT_EQ(site.page_vmaddr - site.page_fileoff, mapping->address);
        EXPECT_EQ(site.page_vmaddr % SWITCH_US_PAGE_SIZE, 0u);
        EXPECT_LE(site.page_vmaddr + SWITCH_US_PAGE_SIZE, mapping->address + mapping->size);
        EXPECT_EQ(site.chunk_offset % kChunk, 0u);
        EXPECT_LT(site.chunk_offset, SWITCH_US_PAGE_SIZE);
        ASSERT_GE(site.edits, 1u);
        ASSERT_LE(site.edits, sizeof(site.edit) / sizeof(site.edit[0]));
        const uint64_t chunk = site.page_fileoff + site.chunk_offset;
        for (unsigned i = 0; i < site.edits; i++) {
            const switch_us_edit &edit = site.edit[i];
            EXPECT_GE(edit.fileoff, chunk) << i;
            EXPECT_LE(edit.fileoff + edit.length, chunk + kChunk) << i;
            EXPECT_LE(edit.length, sizeof(edit.stock)) << i;
            EXPECT_LE(edit.at + edit.size, edit.length) << i;
            if (edit.kind == SWITCH_US_FIXED) {
                EXPECT_LE(edit.size, sizeof(edit.patched)) << i;
            }
            // at scale 1 the scale's kinds write nothing
            EXPECT_TRUE(!site.every_scale || edit.kind == SWITCH_US_FIXED) << i;
        }
    }
}

// switch_shared_cache_text_pager() asks each slot of a mapping once: every site must be the one site of its slot in
// its own mapping, on a slot no other pager answers in that mapping.
inline void ExpectEachSiteTheOneSiteOfItsSlot(const switch_us_cache &cache, const std::vector<TextMapping> &mappings)
{
    const unsigned slots =
        TextSlots(std::string(cache.build) == "24B119" ? "../xnu-12377.121.6/osfmk/vm/vm_shared_region.c"
                                                       : "../xnu-10063.141.1/osfmk/vm/vm_shared_region.c");
    std::vector<unsigned> found(cache.count, 0);
    for (const TextMapping &mapping : mappings) {
        for (unsigned slot = 0; slot < slots; slot++) {
            SCOPED_TRACE(std::string(mapping.name) + " slot " + std::to_string(slot));
            const switch_us_site *site = switch_us_site_for_slot(&cache, slot, mapping.address, mapping.size, 0);
            if (site == nullptr) {
                continue;
            }
            EXPECT_EQ(MappingOf(mappings, *site), &mapping) << NameOf(*site);
            EXPECT_EQ(site->slot, slot) << NameOf(*site);
            EXPECT_EQ(mapping.other_pagers_slots & 1u << slot, 0u)
                << NameOf(*site) << " shares a slot with another pager";
            found[static_cast<size_t>(site - cache.sites)]++;
        }
    }
    for (size_t i = 0; i < cache.count; i++) {
        EXPECT_EQ(found[i], 1u) << NameOf(cache.sites[i]) << " (slot " << cache.sites[i].slot << ")";
    }
    // a mapping that starts after a site's page, ends inside it or maps another file offset holds none of it
    for (size_t i = 0; i < cache.count; i++) {
        const switch_us_site &site = cache.sites[i];
        SCOPED_TRACE(NameOf(site));
        const uint64_t address = MappingOf(mappings, site)->address, size = MappingOf(mappings, site)->size;
        EXPECT_TRUE(switch_us_site_in_mapping(&site, address, size, 0));
        EXPECT_FALSE(switch_us_site_in_mapping(&site, site.page_vmaddr + SWITCH_US_PAGE_SIZE, size, 0));
        EXPECT_FALSE(switch_us_site_in_mapping(&site, address, site.page_vmaddr - address + 0x1000, 0));
        EXPECT_FALSE(switch_us_site_in_mapping(&site, address, size, SWITCH_US_PAGE_SIZE));
    }
}

inline void ExpectNoTwoSitesOfAMappingShareAPage(const switch_us_cache &cache, const std::vector<TextMapping> &mappings)
{
    for (size_t i = 0; i < cache.count; i++) {
        for (size_t j = 0; j < cache.count; j++) {
            const switch_us_site &a = cache.sites[i], &b = cache.sites[j];
            if (i != j && MappingOf(mappings, a) == MappingOf(mappings, b)) {
                EXPECT_NE(a.page_vmaddr, b.page_vmaddr) << NameOf(a) << " and " << NameOf(b);
            }
        }
    }
}

// Every edit made for the scale writes something at 1.25 and at 1.6.
inline void ExpectEditBytesAtEveryScale(const switch_us_cache &cache)
{
    for (size_t s = 0; s < cache.count; s++) {
        const switch_us_site &site = cache.sites[s];
        for (unsigned i = 0; i < site.edits; i++) {
            const switch_us_edit &edit = site.edit[i];
            if (edit.kind == SWITCH_US_FIXED) {
                continue;
            }
            for (const char *name : {"1.25", "1.6"}) {
                uint8_t out[32] = {};
                EXPECT_TRUE(switch_ui_scale_edit_bytes(edit.kind, switch_ui_scale_named(name), out, edit.size))
                    << NameOf(site) << " at " << name;
            }
        }
    }
}

}  // namespace switch_ui_scale_sites

#endif  // SWITCH_XNU_UI_SCALE_SITES_CHECKS_H
