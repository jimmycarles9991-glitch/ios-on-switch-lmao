// kits/SwitchDT over stand-in registry entries: which properties each reader accepts and what it copies.
#include "kits/SwitchDT/SwitchDTProperty.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {
std::vector<unsigned char> bytesOf(const void *data, size_t length)
{
    const auto *p = static_cast<const unsigned char *>(data);
    return {p, p + length};
}

std::vector<unsigned char> bytesOf(const char *text)
{
    return bytesOf(text, strlen(text) + 1);
}

TEST(SwitchDT, MissingNullOrNonDataPropertiesReadAsAbsent)
{
    IORegistryEntry entry;
    entry.setString("name");
    uint32_t value = 7;
    for (const char *key : {"absent", "name"}) {
        EXPECT_FALSE(switch_dt_read(&entry, key, &value, sizeof(value)));
        EXPECT_FALSE(switch_dt_read_prefix(&entry, key, &value, sizeof(value)));
        EXPECT_FALSE(switch_dt_value(&entry, key, &value));
        EXPECT_EQ(switch_dt_data(&entry, key), nullptr);
        EXPECT_EQ(switch_dt_length(&entry, key), 0u);
        EXPECT_EQ(switch_dt_string(&entry, key), nullptr);
        EXPECT_FALSE(switch_dt_string_is(&entry, key, "name"));
    }
    EXPECT_FALSE(switch_dt_value(nullptr, "reg", &value));
    EXPECT_EQ(switch_dt_length(nullptr, "reg"), 0u);
    EXPECT_FALSE(switch_osdata_value(nullptr, &value));
    EXPECT_EQ(value, 7u);
}

TEST(SwitchDT, ExactReadsTakeOnlyTheirLength)
{
    IORegistryEntry entry;
    const uint32_t word = 0x12345678;
    const uint64_t bus[3] = {0x0, 0x70000000, 0x10000000};
    entry.setBytes("word", bytesOf(&word, sizeof(word)));
    entry.setBytes("short", {1, 2, 3});
    entry.setBytes("long", {1, 2, 3, 4, 5});
    entry.setBytes("ranges", bytesOf(bus, sizeof(bus)));

    uint32_t value = 0;
    EXPECT_TRUE(switch_dt_value(&entry, "word", &value));
    EXPECT_EQ(value, word);
    value = 7;
    EXPECT_FALSE(switch_dt_value(&entry, "short", &value));
    EXPECT_FALSE(switch_dt_value(&entry, "long", &value));
    EXPECT_EQ(value, 7u);

    uint64_t ranges[3] = {};
    EXPECT_TRUE(switch_dt_value(&entry, "ranges", &ranges));
    EXPECT_EQ(memcmp(ranges, bus, sizeof(bus)), 0);
    EXPECT_FALSE(switch_dt_read(&entry, "ranges", ranges, 16));
    EXPECT_EQ(switch_dt_length(&entry, "ranges"), sizeof(bus));
}

TEST(SwitchDT, PrefixReadsTakeTheFirstCellOfALongerProperty)
{
    IORegistryEntry entry;
    const uint32_t cells[2] = {0x100, 0x200};
    entry.setBytes("reg", bytesOf(cells, sizeof(cells)));
    entry.setBytes("short", {1, 2, 3});

    uint32_t first = 0;
    EXPECT_TRUE(switch_dt_read_prefix(&entry, "reg", &first, sizeof(first)));
    EXPECT_EQ(first, 0x100u);
    EXPECT_TRUE(switch_osdata_read_prefix(entry.getProperty("reg"), &first, sizeof(first)));
    EXPECT_FALSE(switch_dt_read_prefix(&entry, "short", &first, sizeof(first)));
    EXPECT_FALSE(switch_dt_value(&entry, "reg", &first));
}

TEST(SwitchDT, StringsCompareWholeWithTheirNul)
{
    IORegistryEntry entry;
    entry.setBytes("cpu", bytesOf("cpu"));
    entry.setBytes("longer", bytesOf("cpux"));
    entry.setBytes("unterminated", {'c', 'p', 'u'});
    entry.setBytes("list", {'c', 'p', 'u', 0, 'x', 0});
    entry.setBytes("empty", {});

    EXPECT_TRUE(switch_dt_string_is(&entry, "cpu", "cpu"));
    EXPECT_FALSE(switch_dt_string_is(&entry, "cpu", "cp"));
    EXPECT_FALSE(switch_dt_string_is(&entry, "longer", "cpu"));
    EXPECT_FALSE(switch_dt_string_is(&entry, "unterminated", "cpu"));
    EXPECT_FALSE(switch_dt_string_is(&entry, "list", "cpu"));
    EXPECT_FALSE(switch_dt_string_is(&entry, "empty", ""));

    EXPECT_STREQ(switch_dt_string(&entry, "cpu"), "cpu");
    EXPECT_STREQ(switch_dt_string(&entry, "list"), "cpu");
    EXPECT_EQ(switch_dt_string(&entry, "unterminated"), nullptr);
    EXPECT_EQ(switch_dt_string(&entry, "empty"), nullptr);
}

TEST(SwitchDT, DataIsTheEntrysOwnObject)
{
    IORegistryEntry entry;
    entry.setBytes("patch", {1, 2, 3, 4});
    const OSData *data = switch_dt_data(&entry, "patch");
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data, entry.getProperty("patch"));
    EXPECT_EQ(data->getLength(), 4u);
}
}  // namespace
