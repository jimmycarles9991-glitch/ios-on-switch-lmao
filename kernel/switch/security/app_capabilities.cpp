// SPDX-License-Identifier: GPL-2.0-only
#include <sys/errno.h>
#include <sys/systm.h>
#include <kern/kalloc.h>
#include <libkern/amfi/amfi.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSString.h>
#include <switch/app_code.h>
#include <switch/capabilities.h>
#include <switch/entitlements_factory.h>

static bool switch_capability_string(const OSString *value)
{
    return value && value->getLength() <= SWITCH_CAPABILITIES_MAX_SIZE &&
        strlen(value->getCStringNoCopy()) == value->getLength();
}

static bool switch_capability_value(OSObject *value, unsigned depth)
{
    if (depth > SWITCH_CAPABILITIES_MAX_DEPTH) return false;
    if (auto string = OSDynamicCast(OSString, value)) return switch_capability_string(string);
    if (OSDynamicCast(OSBoolean, value) || OSDynamicCast(OSNumber, value)) return true;
    if (auto array = OSDynamicCast(OSArray, value)) {
        for (unsigned i = 0; i < array->getCount(); ++i)
            if (!switch_capability_value(array->getObject(i), depth + 1)) return false;
        return true;
    }
    if (auto dictionary = OSDynamicCast(OSDictionary, value)) {
        auto iterator = OSCollectionIterator::withCollection(dictionary);
        if (!iterator) return false;
        bool valid = true;
        while (auto key = OSDynamicCast(OSString, iterator->getNextObject())) {
            if (!switch_capability_string(key) ||
                !switch_capability_value(dictionary->getObject(key), depth + 1)) {
                valid = false;
                break;
            }
        }
        iterator->release();
        return valid;
    }
    return false;
}

static bool switch_capability_dictionary(OSDictionary *dictionary)
{
    if (!dictionary || !switch_capability_value(dictionary, 0)) return false;
    auto iterator = OSCollectionIterator::withCollection(dictionary);
    if (!iterator) return false;
    bool valid = true;
    while (auto key = OSDynamicCast(OSString, iterator->getNextObject())) {
        const char *name = key->getCStringNoCopy();
        OSObject *value = dictionary->getObject(key);
        if (!switch_capability_key_allowed(name)) {
            valid = false;
            break;
        }
        if (switch_capability_identity_key(name)) {
            auto string = OSDynamicCast(OSString, value);
            if (!string || !switch_capability_group_allowed(string->getCStringNoCopy())) {
                valid = false;
                break;
            }
        } else if (switch_capability_group_key(name)) {
            auto array = OSDynamicCast(OSArray, value);
            if (!array) {
                valid = false;
                break;
            }
            for (unsigned i = 0; i < array->getCount(); ++i) {
                auto string = OSDynamicCast(OSString, array->getObject(i));
                if (!string || !switch_capability_group_allowed(string->getCStringNoCopy())) {
                    valid = false;
                    break;
                }
            }
            if (!valid) break;
        }
    }
    iterator->release();
    return valid;
}

int switch_app_code_capability_decode(const void *data, size_t size, struct switch_app_code_capabilities *result)
{
    if (!amfi || !data || !size || size > SWITCH_CAPABILITIES_MAX_SIZE) return EINVAL;
    auto object = static_cast<OSObject *>(switch_app_code_native_dictionary(data, size));
    auto dictionary = OSDynamicCast(OSDictionary, object);
    int error = EINVAL;
    if (switch_capability_dictionary(dictionary)) {
        auto identity = OSDynamicCast(OSString, dictionary->getObject("application-identifier"));
        if (identity && identity->getLength() < sizeof(result->identity)) {
            auto team = OSDynamicCast(OSString, dictionary->getObject("com.apple.developer.team-identifier"));
            if (team && team->getLength() >= sizeof(result->team)) {
                object->release();
                return EINVAL;
            }
            if (team) strlcpy(result->team, team->getCStringNoCopy(), sizeof(result->team));
            strlcpy(result->identity, identity->getCStringNoCopy(), sizeof(result->identity));
            auto serialized = OSSerialize::withCapacity(1024);
            if (!serialized) error = ENOMEM;
            else {
                if (dictionary->serialize(serialized) && serialized->getLength() <= 1024 * 1024) {
                    result->xml_size = serialized->getLength();
                    result->xml = kalloc_data(result->xml_size, Z_WAITOK);
                    if (result->xml) {
                        memcpy(result->xml, serialized->text(), result->xml_size);
                        error = 0;
                    } else error = ENOMEM;
                }
                serialized->release();
            }
        }
    }
    if (object) object->release();
    return error;
}
