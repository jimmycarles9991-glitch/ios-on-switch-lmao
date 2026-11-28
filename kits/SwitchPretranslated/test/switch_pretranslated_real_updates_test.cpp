// The kernel's update check (switch_pt_signed_as_cdhash, which pretranslated_updates.c runs at capture) over the real
// translated files the boot package's pins name, from //install:pretranslated_updates: the bytes a deploy puts on the
// card. iOS 27 only.
#include <CommonCrypto/CommonDigest.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "kits/SwitchPretranslated/switch_pretranslated_updates.h"

namespace {

std::string Env(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : "";
}

// {System path: CDHash hex} of a pins file ({"/usr/libexec/locationd": "020e95fc..."}).
std::map<std::string, std::string> Pins(const std::string &path)
{
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    const std::string json = text.str();
    std::map<std::string, std::string> pins;
    const std::regex entry(R"re("(/[^"]+)"\s*:\s*"([0-9a-f]{40})")re");
    for (std::sregex_iterator it(json.begin(), json.end(), entry), end; it != end; ++it)
        pins[(*it)[1].str()] = (*it)[2].str();
    return pins;
}

std::array<uint8_t, SWITCH_PT_CDHASH> Bytes(const std::string &hex)
{
    std::array<uint8_t, SWITCH_PT_CDHASH> out{};
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    return out;
}

bool ReadAt(void *context, void *buffer, uint32_t size, uint64_t offset)
{
    auto *file = static_cast<std::FILE *>(context);
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0 && std::fread(buffer, 1, size, file) == size;
}

void *Allocate(size_t size)
{
    return std::malloc(size);
}

void Release(void *bytes, size_t)
{
    std::free(bytes);
}

void Sha256(const void *bytes, size_t size, uint8_t digest[SWITCH_PT_SHA256])
{
    CC_SHA256(bytes, static_cast<CC_LONG>(size), digest);
}

std::vector<uint8_t> FileBytes(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

uint32_t U32(const std::vector<uint8_t> &bytes, size_t at)
{
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, sizeof(value));
    return value;
}

// An Apple device tree node at `at`: its properties {name: value, as long as its length word says} and the offset
// past it (properties padded to 4 bytes, then the children).
size_t Node(const std::vector<uint8_t> &adt, size_t at, std::map<std::string, std::vector<uint8_t>> *properties,
            std::vector<size_t> *children)
{
    const uint32_t count = U32(adt, at), nodes = U32(adt, at + 4);
    at += 8;
    for (uint32_t i = 0; i < count; ++i) {
        const std::string name(reinterpret_cast<const char *>(adt.data() + at),
                               strnlen(reinterpret_cast<const char *>(adt.data() + at), 32));
        const uint32_t length = U32(adt, at + 32) & 0x7fffffffu;
        if (properties)
            (*properties)[name].assign(adt.begin() + static_cast<long>(at + 36),
                                       adt.begin() + static_cast<long>(at + 36 + length));
        at += 36 + length + (-length & 3u);
    }
    for (uint32_t i = 0; i < nodes; ++i) {
        if (children)
            children->push_back(at);
        at = Node(adt, at, nullptr, nullptr);
    }
    return at;
}

// The root's child named `name`: its properties.
std::map<std::string, std::vector<uint8_t>> Child(const std::vector<uint8_t> &adt, const char *name)
{
    std::vector<size_t> children;
    Node(adt, 0, nullptr, &children);
    for (const size_t at : children) {
        std::map<std::string, std::vector<uint8_t>> properties;
        Node(adt, at, &properties, nullptr);
        const std::vector<uint8_t> &own = properties["name"];
        if (std::string(own.begin(), own.end()).c_str() == std::string(name))
            return properties;
    }
    return {};
}

}  // namespace

TEST(RealUpdates, TheBootPackagesUpdateListParsesAsThePinsName)
{
    const std::vector<uint8_t> adt = FileBytes(Env("SWITCH_ADT"));
    ASSERT_GT(adt.size(), 8u) << Env("SWITCH_ADT");
    std::map<std::string, std::vector<uint8_t>> chosen = Child(adt, "chosen");
    ASSERT_TRUE(chosen.count("switch-pretranslated-updates")) << "no /chosen/switch-pretranslated-updates";
    const std::vector<uint8_t> &raw = chosen["switch-pretranslated-updates"];
    struct switch_pt_updates list;
    ASSERT_EQ(switch_pt_updates_parse(raw.data(), raw.size(), &list), SWITCH_PT_OK);
    const std::map<std::string, std::string> pins = Pins(Env("SWITCH_UPDATES_PINS"));
    ASSERT_EQ(list.count, pins.size());
    for (uint32_t i = 0; i < list.count; ++i) {
        const struct switch_pt_update update = switch_pt_update_at(&list, i);
        const auto pin = pins.find(update.path);
        ASSERT_NE(pin, pins.end()) << update.path;
        const std::array<uint8_t, SWITCH_PT_CDHASH> cdhash = Bytes(pin->second);
        EXPECT_EQ(std::memcmp(update.cdhash, cdhash.data(), SWITCH_PT_CDHASH), 0) << update.path;
    }
}

TEST(RealUpdates, EveryPinnedUpdateIsSignedAsItsCDHashWithinTheHeadTheKernelReads)
{
    const std::string tree = Env("SWITCH_UPDATES_TREE");
    const std::map<std::string, std::string> pins = Pins(Env("SWITCH_UPDATES_PINS"));
    ASSERT_FALSE(pins.empty()) << "no updates in " << Env("SWITCH_UPDATES_PINS");
    for (const auto &[path, hex] : pins) {
        const std::string file = tree + path;
        std::FILE *bytes = std::fopen(file.c_str(), "rb");
        ASSERT_NE(bytes, nullptr) << file;
        struct stat st{};
        ASSERT_EQ(fstat(fileno(bytes), &st), 0) << file;
        std::array<uint8_t, 32> header{};
        ASSERT_TRUE(ReadAt(bytes, header.data(), header.size(), 0)) << file;
        uint32_t commands = 0;
        std::memcpy(&commands, header.data() + 20, sizeof(commands));
        EXPECT_LE(32u + commands, SWITCH_PT_UPDATE_HEAD) << path << ": load commands past the head the kernel reads";
        RecordProperty(path + " load command bytes", static_cast<int>(commands));
        const struct switch_pt_reader reader = {bytes, ReadAt, Allocate, Release, Sha256};
        const std::array<uint8_t, SWITCH_PT_CDHASH> cdhash = Bytes(hex);
        EXPECT_TRUE(switch_pt_signed_as_cdhash(&reader, static_cast<uint64_t>(st.st_size), cdhash.data())) << path;
        std::array<uint8_t, SWITCH_PT_CDHASH> other = cdhash;
        other[0] ^= 1;
        EXPECT_FALSE(switch_pt_signed_as_cdhash(&reader, static_cast<uint64_t>(st.st_size), other.data())) << path;
        std::fclose(bytes);
    }
}
