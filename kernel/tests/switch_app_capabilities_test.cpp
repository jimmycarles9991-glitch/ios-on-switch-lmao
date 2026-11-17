#include <gtest/gtest.h>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../switch/include/switch/capabilities.h"

namespace {
struct OSObject { virtual ~OSObject() = default; };
struct OSString : OSObject {
    std::string value;
    explicit OSString(std::string text) : value(std::move(text)) {}
    size_t getLength() const { return value.size(); }
    const char *getCStringNoCopy() const { return value.c_str(); }
};
struct OSBoolean : OSObject {};
struct OSNumber : OSObject {};
struct OSData : OSObject {};
struct OSArray : OSObject {
    std::vector<OSObject *> values;
    unsigned getCount() const { return static_cast<unsigned>(values.size()); }
    OSObject *getObject(unsigned index) const { return values.at(index); }
};
struct OSDictionary : OSObject {
    struct Entry { OSString key; OSObject *value; };
    std::map<std::string, Entry> entries;
    void set(const char *key, OSObject *value) { entries.emplace(key, Entry{OSString(key), value}); }
    OSObject *getObject(const OSString *key) { return entries.at(key->value).value; }
};
struct OSCollectionIterator {
    std::vector<OSObject *> keys;
    size_t index = 0;
    static OSCollectionIterator *withCollection(OSDictionary *dictionary)
    {
        auto iterator = new OSCollectionIterator;
        for (auto &[name, entry] : dictionary->entries) {
            (void)name;
            iterator->keys.push_back(&entry.key);
        }
        return iterator;
    }
    OSObject *getNextObject() { return index < keys.size() ? keys[index++] : nullptr; }
    void release() { delete this; }
};
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#include "app_capability_validation.h"
#undef OSDynamicCast
}

TEST(SwitchAppCapabilities, PreservesOrdinaryDeclaredCapabilitiesAndGroupNames)
{
    OSString identity("TEAM.example.app"), team("TEAM"), group("group.example.shared");
    OSString domain("applinks:*.example.com"), topic("development");
    OSArray groups, domains;
    groups.values = {&group}; domains.values = {&domain};
    OSBoolean enabled;
    OSDictionary dictionary;
    dictionary.set("application-identifier", &identity);
    dictionary.set("com.apple.developer.team-identifier", &team);
    dictionary.set("keychain-access-groups", &groups);
    dictionary.set("com.apple.security.application-groups", &groups);
    dictionary.set("com.apple.developer.associated-domains", &domains);
    dictionary.set("aps-environment", &topic);
    dictionary.set("org.example.custom-capability", &enabled);
    EXPECT_TRUE(switch_capability_dictionary(&dictionary));
    EXPECT_EQ(group.value, "group.example.shared");
}

TEST(SwitchAppCapabilities, RejectsPrivilegeKeysAppleGroupsAndWildcardGroups)
{
    OSBoolean enabled;
    for (const char *key : {"platform-application", "get-task-allow", "no-sandbox", "allow-root",
                           "com.apple.private.security.no-sandbox", "com.apple.developer.driverkit"}) {
        OSDictionary dictionary; dictionary.set(key, &enabled);
        EXPECT_FALSE(switch_capability_dictionary(&dictionary)) << key;
    }
    for (const char *name : {"apple", "group.apple", "com.apple.token", "group.com.apple.shared",
                            "TEAM.com.apple.token", "systemgroup.any", "group.example.*"}) {
        OSString group(name); OSArray groups; groups.values = {&group};
        OSDictionary dictionary; dictionary.set("keychain-access-groups", &groups);
        EXPECT_FALSE(switch_capability_dictionary(&dictionary)) << name;
    }
}

TEST(SwitchAppCapabilities, RejectsMalformedTypesEmbeddedNullsAndDeepContainers)
{
    OSData data;
    OSDictionary dictionary; dictionary.set("org.example.opaque", &data);
    EXPECT_FALSE(switch_capability_dictionary(&dictionary));
    OSString embedded(std::string("valid\0hidden", 12));
    EXPECT_FALSE(switch_capability_value(&embedded, 0));
    OSString group("group.example");
    OSDictionary wrong; wrong.set("keychain-access-groups", &group);
    EXPECT_FALSE(switch_capability_dictionary(&wrong));
    std::vector<std::unique_ptr<OSArray>> arrays;
    for (unsigned depth = 0; depth <= SWITCH_CAPABILITIES_MAX_DEPTH + 1; ++depth) {
        auto array = std::make_unique<OSArray>();
        if (!arrays.empty()) array->values.push_back(arrays.back().get());
        arrays.push_back(std::move(array));
    }
    EXPECT_FALSE(switch_capability_value(arrays.back().get(), 0));
    EXPECT_TRUE(switch_capability_value(arrays[SWITCH_CAPABILITIES_MAX_DEPTH].get(), 0));
}
