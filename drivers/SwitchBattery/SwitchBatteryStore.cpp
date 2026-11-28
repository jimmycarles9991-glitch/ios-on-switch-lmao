// SPDX-License-Identifier: BSD-2-Clause-Patent
// powerd's DateOfFirstUse and ShipChargeLimitData, kept in NVRAM (SwitchBatteryStore.h). 24B119 only.
#include <IOKit/IOLib.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <string.h>
#include "SwitchBatteryStore.h"

#if SWITCH_IPSW_KIND_24B119
namespace {

enum : uint8_t {
    kBoolean = 1,
    kNumber = 2,
    kString = 3,
};

struct Writer {
    uint8_t *out;
    size_t capacity, at;
    unsigned count;
    bool overflow;

    void put(const void *bytes, size_t n)
    {
        if (overflow || at + n > capacity) {
            overflow = true;
            return;
        }
        __builtin_memcpy(out + at, bytes, n);
        at += n;
    }

    void put(uint8_t byte)
    {
        put(&byte, 1);
    }
};

bool putEntry(void *refcon, const OSSymbol *key, OSObject *object)
{
    auto *w = static_cast<Writer *>(refcon);
    const char *name = key->getCStringNoCopy();
    const size_t length = strlen(name);
    if (w->count == 255)
        return true;
    if (length > 255)
        return false;
    if (const OSBoolean *b = OSDynamicCast(OSBoolean, object)) {
        w->put(static_cast<uint8_t>(length));
        w->put(name, length);
        w->put(kBoolean);
        w->put(static_cast<uint8_t>(b->getValue() ? 1 : 0));
    } else if (const OSNumber *n = OSDynamicCast(OSNumber, object)) {
        const uint64_t value = n->unsigned64BitValue();
        w->put(static_cast<uint8_t>(length));
        w->put(name, length);
        w->put(kNumber);
        for (unsigned i = 0; i < 8; ++i)
            w->put(static_cast<uint8_t>(value >> (8 * i)));
    } else if (const OSString *s = OSDynamicCast(OSString, object)) {
        const size_t textLength = strlen(s->getCStringNoCopy());
        if (textLength > 255)
            return false;
        w->put(static_cast<uint8_t>(length));
        w->put(name, length);
        w->put(kString);
        w->put(static_cast<uint8_t>(textLength));
        w->put(s->getCStringNoCopy(), textLength);
    } else {
        return false;
    }
    ++w->count;
    return false;
}

struct Reader {
    const uint8_t *in;
    size_t length, at;

    bool take(size_t n)
    {
        if (at + n > length)
            return false;
        at += n;
        return true;
    }
};

bool readEntry(Reader &r, OSDictionary *dict)
{
    char key[256];
    const size_t keyAt = r.at + 1;
    if (!r.take(1))
        return false;
    const size_t keyLength = r.in[keyAt - 1];
    if (!r.take(keyLength + 1))
        return false;
    __builtin_memcpy(key, r.in + keyAt, keyLength);
    key[keyLength] = '\0';
    const uint8_t type = r.in[keyAt + keyLength];
    const size_t valueAt = r.at;
    switch (type) {
    case kBoolean:
        if (!r.take(1))
            return false;
        return dict->setObject(key, r.in[valueAt] ? kOSBooleanTrue : kOSBooleanFalse);
    case kNumber: {
        if (!r.take(8))
            return false;
        uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i)
            value |= static_cast<uint64_t>(r.in[valueAt + i]) << (8 * i);
        OSNumber *n = OSNumber::withNumber(value, 64);
        const bool ok = n && dict->setObject(key, n);
        OSSafeReleaseNULL(n);
        return ok;
    }
    case kString: {
        if (!r.take(1))
            return false;
        const size_t textLength = r.in[valueAt];
        char text[256];
        if (!r.take(textLength))
            return false;
        __builtin_memcpy(text, r.in + valueAt + 1, textLength);
        text[textLength] = '\0';
        OSString *s = OSString::withCString(text);
        const bool ok = s && dict->setObject(key, s);
        OSSafeReleaseNULL(s);
        return ok;
    }
    default:
        return false;
    }
}

// The entry at `at` in an encoding: its key, and its type with its value; `at` moves past it.
struct Entry {
    const uint8_t *key, *value;
    size_t keyLength, valueLength;
};

bool nextEntry(const uint8_t *in, size_t length, size_t &at, Entry &e)
{
    if (at >= length)
        return false;
    e.keyLength = in[at];
    e.key = in + at + 1;
    const size_t typeAt = at + 1 + e.keyLength;
    if (typeAt >= length)
        return false;
    switch (in[typeAt]) {
    case kBoolean:
        e.valueLength = 2;
        break;
    case kNumber:
        e.valueLength = 9;
        break;
    case kString:
        if (typeAt + 1 >= length)
            return false;
        e.valueLength = 2 + in[typeAt + 1];
        break;
    default:
        return false;
    }
    e.value = in + typeAt;
    if (typeAt + e.valueLength > length)
        return false;
    at = typeAt + e.valueLength;
    return true;
}

bool sameEntry(const Entry &a, const Entry &b)
{
    return a.keyLength == b.keyLength && a.valueLength == b.valueLength &&
           __builtin_memcmp(a.key, b.key, a.keyLength) == 0 && __builtin_memcmp(a.value, b.value, a.valueLength) == 0;
}

}  // namespace

// A dictionary's keys are unique, so equal counts and every entry of `a` found in `b` make the same entries.
bool switch_battery_store_same(const uint8_t *a, size_t aLength, const uint8_t *b, size_t bLength)
{
    if (aLength == 0 || bLength == 0 || a[0] != b[0])
        return false;
    size_t at = 1;
    for (unsigned i = 0; i < a[0]; ++i) {
        Entry ea{};
        if (!nextEntry(a, aLength, at, ea))
            return false;
        bool found = false;
        size_t bt = 1;
        for (unsigned j = 0; j < b[0] && !found; ++j) {
            Entry eb{};
            if (!nextEntry(b, bLength, bt, eb))
                return false;
            found = sameEntry(ea, eb);
        }
        if (!found)
            return false;
    }
    return at == aLength;
}

size_t switch_battery_store_encode(const OSDictionary *dict, uint8_t *out, size_t capacity)
{
    Writer w = {out, capacity, 1, 0, false};
    if (!dict || capacity == 0)
        return 0;
    // iterateObjects is not const in libkern; the walk changes nothing.
    const_cast<OSDictionary *>(dict)->iterateObjects(&w, putEntry);
    if (w.overflow)
        return 0;
    out[0] = static_cast<uint8_t>(w.count);
    return w.at;
}

OSDictionary *switch_battery_store_decode(const uint8_t *in, size_t length)
{
    if (length == 0)
        return nullptr;
    OSDictionary *dict = OSDictionary::withCapacity(in[0]);
    Reader r = {in, length, 1};
    for (unsigned i = 0; dict && i < in[0]; ++i) {
        if (!readEntry(r, dict))
            OSSafeReleaseNULL(dict);
    }
    if (dict && r.at != length)
        OSSafeReleaseNULL(dict);
    return dict;
}

void switch_battery_store_load(uint64_t *dateOfFirstUse, OSDictionary **shipChargeLimit)
{
    uint8_t bytes[SWITCH_BATTERY_SHIP_LIMIT_MAX];
    size_t length = 8;
    *dateOfFirstUse = 0;
    *shipChargeLimit = nullptr;
    if (switch_battery_nvram_read(SWITCH_BATTERY_NVRAM_DATE_OF_FIRST_USE, bytes, &length) && length == 8) {
        for (unsigned i = 0; i < 8; ++i)
            *dateOfFirstUse |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    length = sizeof(bytes);
    if (switch_battery_nvram_read(SWITCH_BATTERY_NVRAM_SHIP_CHARGE_LIMIT, bytes, &length)) {
        *shipChargeLimit = switch_battery_store_decode(bytes, length);
        if (!*shipChargeLimit)
            IOLog("SwitchBattery: NVRAM %s does not parse (%zu bytes): ShipChargeLimitData not restored\n",
                  SWITCH_BATTERY_NVRAM_SHIP_CHARGE_LIMIT, length);
    }
}

bool switch_battery_store_save(uint64_t dateOfFirstUse, const OSDictionary *shipChargeLimit)
{
    bool ok = true;
    if (dateOfFirstUse) {
        uint8_t bytes[8];
        for (unsigned i = 0; i < 8; ++i)
            bytes[i] = static_cast<uint8_t>(dateOfFirstUse >> (8 * i));
        ok = switch_battery_nvram_write(SWITCH_BATTERY_NVRAM_DATE_OF_FIRST_USE, bytes, sizeof(bytes)) && ok;
    }
    if (shipChargeLimit) {
        uint8_t bytes[SWITCH_BATTERY_SHIP_LIMIT_MAX];
        const size_t length = switch_battery_store_encode(shipChargeLimit, bytes, sizeof(bytes));
        ok = length != 0 && switch_battery_nvram_write(SWITCH_BATTERY_NVRAM_SHIP_CHARGE_LIMIT, bytes, length) && ok;
    }
    return ok;
}
#endif
