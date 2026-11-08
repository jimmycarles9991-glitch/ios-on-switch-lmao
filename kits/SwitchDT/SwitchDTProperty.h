// SPDX-License-Identifier: BSD-2-Clause-Patent
// Registry properties as the device tree stores them: OSData bytes, native-endian, strings with their NUL.
//
// switch_dt_* look a key up with getProperty(), which hands back an unretained object: right for device-tree nodes,
// whose properties never change. For a live service's property, copyProperty() it and read it with switch_osdata_*.
#ifndef SWITCH_DT_PROPERTY_H
#define SWITCH_DT_PROPERTY_H

#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <stddef.h>
#include <string.h>

inline const OSData *switch_osdata(const OSObject *value)
{
    return OSDynamicCast(OSData, value);
}

inline size_t switch_osdata_length(const OSObject *value)
{
    const OSData *data = switch_osdata(value);
    return data ? data->getLength() : 0;
}

inline bool switch_osdata_read(const OSObject *value, void *out, size_t length)
{
    const OSData *data = switch_osdata(value);
    if (!data || data->getLength() != length)
        return false;
    memcpy(out, data->getBytesNoCopy(), length);
    return true;
}

// The first `length` bytes of a property that may be longer (a cell list read for its first cell).
inline bool switch_osdata_read_prefix(const OSObject *value, void *out, size_t length)
{
    const OSData *data = switch_osdata(value);
    if (!data || data->getLength() < length)
        return false;
    memcpy(out, data->getBytesNoCopy(), length);
    return true;
}

template <typename T> inline bool switch_osdata_value(const OSObject *value, T *out)
{
    return switch_osdata_read(value, out, sizeof(*out));
}

inline const char *switch_osdata_string(const OSObject *value)
{
    const OSData *data = switch_osdata(value);
    if (!data || data->getLength() == 0)
        return nullptr;
    const char *bytes = static_cast<const char *>(data->getBytesNoCopy());
    return bytes[data->getLength() - 1] == '\0' ? bytes : nullptr;
}

// One whole string: a list of several ("a\0b\0") never equals its first.
inline bool switch_osdata_string_is(const OSObject *value, const char *expected)
{
    const OSData *data = switch_osdata(value);
    const size_t length = strlen(expected) + 1;
    return data != nullptr && data->getLength() == length && memcmp(data->getBytesNoCopy(), expected, length) == 0;
}

inline const OSObject *switch_dt_property(const IORegistryEntry *entry, const char *key)
{
    return entry ? entry->getProperty(key) : nullptr;
}

inline const OSData *switch_dt_data(const IORegistryEntry *entry, const char *key)
{
    return switch_osdata(switch_dt_property(entry, key));
}

inline size_t switch_dt_length(const IORegistryEntry *entry, const char *key)
{
    return switch_osdata_length(switch_dt_property(entry, key));
}

inline bool switch_dt_read(const IORegistryEntry *entry, const char *key, void *out, size_t length)
{
    return switch_osdata_read(switch_dt_property(entry, key), out, length);
}

inline bool switch_dt_read_prefix(const IORegistryEntry *entry, const char *key, void *out, size_t length)
{
    return switch_osdata_read_prefix(switch_dt_property(entry, key), out, length);
}

template <typename T> inline bool switch_dt_value(const IORegistryEntry *entry, const char *key, T *out)
{
    return switch_osdata_value(switch_dt_property(entry, key), out);
}

inline const char *switch_dt_string(const IORegistryEntry *entry, const char *key)
{
    return switch_osdata_string(switch_dt_property(entry, key));
}

inline bool switch_dt_string_is(const IORegistryEntry *entry, const char *key, const char *expected)
{
    return switch_osdata_string_is(switch_dt_property(entry, key), expected);
}

#endif
