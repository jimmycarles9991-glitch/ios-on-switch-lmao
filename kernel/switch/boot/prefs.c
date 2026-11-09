// SPDX-License-Identifier: GPL-2.0-only
/*
 * <switch/prefs.h>. A binary plist's top-level dictionary (CoreFoundation CFBinaryPList.c) is the marker 0xDn (n
 * entries; 0xDF: an integer object 0x1k with 2^k bytes of count follows), its n key references, then its n value
 * references, each the trailer's object reference size, big-endian. Writing only the kept entries where the dictionary
 * is leaves every object, the offset table and the trailer as they were.
 */
#include <switch/prefs.h>

#include <stdbool.h>
#include <string.h>

#define PREFS_MAX_KEYS 8u

enum {
    kMagic = 8,
    kTrailer = 32
};

static uint64_t be(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        v = v << 8 | p[i];
    }
    return v;
}

static void put_be(uint8_t *p, size_t n, uint64_t v)
{
    for (size_t i = n; i-- > 0; v >>= 8) {
        p[i] = (uint8_t)v;
    }
}

struct bplist {
    const uint8_t *bytes;
    size_t objects_end; /* the offset table: every object lies before it */
    const uint8_t *offsets;
    size_t offset_size, ref_size;
    uint64_t object_count;
};

/* Object `ref`'s offset; 0 when there is no such object. */
static size_t object_at(const struct bplist *b, uint64_t ref)
{
    if (ref >= b->object_count) {
        return 0;
    }
    const uint64_t offset = be(b->offsets + ref * b->offset_size, b->offset_size);
    return offset >= kMagic && offset < b->objects_end ? (size_t)offset : 0;
}

/* The count of the object at `at`: returns the bytes its marker and count take, 0 when unreadable. */
static size_t count_at(const struct bplist *b, size_t at, uint64_t *count)
{
    const uint8_t nibble = (uint8_t)(b->bytes[at] & 0xf);
    if (nibble != 0xf) {
        *count = nibble;
        return 1;
    }
    if (at + 2 > b->objects_end || (b->bytes[at + 1] & 0xf0) != 0x10 || (b->bytes[at + 1] & 0xf) > 3) {
        return 0;
    }
    const size_t width = (size_t)1 << (b->bytes[at + 1] & 0xf);
    if (at + 2 + width > b->objects_end) {
        return 0;
    }
    *count = be(b->bytes + at + 2, width);
    return 2 + width;
}

static bool key_is(const struct bplist *b, uint64_t ref, const char *key)
{
    const size_t at = object_at(b, ref);
    uint64_t length = 0;
    size_t header = 0;
    if (!at || (b->bytes[at] & 0xf0) != 0x50 || !(header = count_at(b, at, &length))) {
        return false;
    }
    return length == strlen(key) && length <= b->objects_end - at - header &&
           !memcmp(b->bytes + at + header, key, (size_t)length);
}

/* The trailer and the top-level dictionary of a binary plist: false when this is not one it can read. */
struct top_dict {
    uint64_t top, entries;
    size_t at, header;
};

static bool read_binary(const uint8_t *plist, size_t length, struct bplist *b, struct top_dict *d)
{
    if (length < kMagic + 1 + kTrailer) {
        return false;
    }
    const uint8_t *trailer = plist + length - kTrailer;
    *b = (struct bplist){
        .bytes = plist,
        .offset_size = trailer[6],
        .ref_size = trailer[7],
        .object_count = be(trailer + 8, 8),
    };
    d->top = be(trailer + 16, 8);
    const uint64_t table = be(trailer + 24, 8);
    if (b->offset_size < 1 || b->offset_size > 8 || b->ref_size < 1 || b->ref_size > 8 || table < kMagic + 1 ||
        table > length - kTrailer || !b->object_count ||
        b->object_count > (length - kTrailer - table) / b->offset_size) {
        return false;
    }
    b->objects_end = (size_t)table;
    b->offsets = plist + table;
    d->at = object_at(b, d->top);
    return d->at && (plist[d->at] & 0xf0) == 0xd0 && (d->header = count_at(b, d->at, &d->entries)) &&
           d->entries <= (b->objects_end - d->at - d->header) / (2 * b->ref_size);
}

static enum switch_prefs_result drop_binary(uint8_t *plist, size_t length, const char *const *keys, size_t count)
{
    struct bplist b;
    struct top_dict d;
    if (!read_binary(plist, length, &b, &d)) {
        return SWITCH_PREFS_UNREADABLE;
    }
    const uint64_t top = d.top, entries = d.entries;
    const size_t at = d.at, header = d.header;
    const size_t rs = b.ref_size, end = at + header + (size_t)entries * 2 * rs;
    /* The edit rewrites the dictionary's bytes: no other object may claim any of them (a damaged offset table). */
    for (uint64_t i = 0; i < b.object_count; ++i) {
        const size_t other = object_at(&b, i);
        if (i != top && other >= at && other < end) {
            return SWITCH_PREFS_UNREADABLE;
        }
    }
    uint64_t dropped[PREFS_MAX_KEYS];
    size_t drops = 0;
    for (uint64_t i = 0; i < entries; ++i) {
        const uint64_t key = be(plist + at + header + i * rs, rs);
        for (size_t k = 0; k < count; ++k) {
            if (key_is(&b, key, keys[k])) {
                if (drops == count) {
                    return SWITCH_PREFS_UNREADABLE; /* a key twice */
                }
                dropped[drops++] = i;
                break;
            }
        }
    }
    if (!drops) {
        return SWITCH_PREFS_UNCHANGED;
    }
    const uint64_t kept = entries - drops;
    if (!kept) {
        return SWITCH_PREFS_EMPTY;
    }

    /* Every kept reference moves to a lower offset or stays: one forward pass reads each before anything lands on it. */
    size_t new_header = 1;
    if (kept < 15) {
        plist[at] = (uint8_t)(0xd0 | kept);
    } else {
        new_header = header;
        put_be(plist + at + 2, header - 2, kept);
    }
    for (size_t half = 0; half < 2; ++half) {
        size_t to = at + new_header + half * (size_t)kept * rs, next = 0;
        for (uint64_t i = 0; i < entries; ++i) {
            if (next < drops && dropped[next] == i) {
                ++next;
                continue;
            }
            memmove(plist + to, plist + at + header + (half * entries + i) * rs, rs);
            to += rs;
        }
    }
    return SWITCH_PREFS_DROPPED;
}

static size_t find(const uint8_t *text, size_t length, size_t from, const char *needle, size_t size)
{
    for (size_t at = from; size <= length && at <= length - size; ++at) {
        if (!memcmp(text + at, needle, size)) {
            return at;
        }
    }
    return length;
}

static enum switch_prefs_result drop_xml(uint8_t *plist, size_t *length, const char *const *keys, size_t count)
{
    static const char *const values[] = {"<true/>", "<false/>"};
    size_t n = *length;
    bool dropped = false;
    for (size_t k = 0; k < count; ++k) {
        char tag[96];
        const size_t name = strlen(keys[k]), size = name + 11;
        if (size > sizeof(tag)) {
            return SWITCH_PREFS_UNREADABLE;
        }
        memcpy(tag, "<key>", 5);
        memcpy(tag + 5, keys[k], name);
        memcpy(tag + 5 + name, "</key>", 6);
        for (size_t at = find(plist, n, 0, tag, size); at < n; at = find(plist, n, at, tag, size)) {
            size_t span = 0;
            for (size_t v = 0; v < sizeof(values) / sizeof(values[0]) && !span; ++v) {
                const size_t value = strlen(values[v]);
                if (n - at - size >= value && !memcmp(plist + at + size, values[v], value)) {
                    span = size + value;
                }
            }
            if (!span) {
                return SWITCH_PREFS_UNREADABLE;
            }
            memmove(plist + at, plist + at + span, n - at - span);
            n -= span;
            dropped = true;
        }
    }
    if (!dropped) {
        return SWITCH_PREFS_UNCHANGED;
    }
    if (find(plist, n, 0, "<key>", 5) == n) {
        return SWITCH_PREFS_EMPTY;
    }
    *length = n;
    return SWITCH_PREFS_DROPPED;
}

enum switch_prefs_result switch_prefs_drop_keys(uint8_t *plist, size_t *length, const char *const *keys, size_t count)
{
    if (!plist || !length || !count || count > PREFS_MAX_KEYS) {
        return SWITCH_PREFS_UNREADABLE;
    }
    if (*length >= kMagic && !memcmp(plist, "bplist00", kMagic)) {
        return drop_binary(plist, *length, keys, count);
    }
    const size_t head = *length < 256 ? *length : 256;
    if (find(plist, head, 0, "<plist", 6) < head) {
        return drop_xml(plist, length, keys, count);
    }
    return SWITCH_PREFS_UNREADABLE;
}

#define PREFS_ADD_MAX_LENGTH (64u * 1024u)

/* An integer object: 0x1k then 2^k bytes, big-endian; CoreFoundation reads 1, 2 and 4 bytes unsigned and 8 signed.
 * Written at `p` unless it is NULL; returns its size either way. */
static size_t put_integer(uint8_t *p, int64_t value)
{
    size_t width = 8;
    uint8_t power = 3;
    if (value >= 0 && value <= 0xff) {
        width = 1, power = 0;
    } else if (value >= 0 && value <= 0xffff) {
        width = 2, power = 1;
    } else if (value >= 0 && value <= 0xffffffffLL) {
        width = 4, power = 2;
    }
    if (p) {
        p[0] = (uint8_t)(0x10 | power);
        put_be(p + 1, width, (uint64_t)value);
    }
    return 1 + width;
}

/* A string, dictionary or array marker with its count: 0xMn, or 0xMF and an integer object when n is 15 or more. */
static size_t put_marker(uint8_t *p, uint8_t kind, uint64_t count)
{
    if (count < 15) {
        if (p) {
            p[0] = (uint8_t)(kind | count);
        }
        return 1;
    }
    if (p) {
        p[0] = (uint8_t)(kind | 0xf);
    }
    return 1 + put_integer(p ? p + 1 : NULL, (int64_t)count);
}

static size_t offset_width(uint64_t largest)
{
    size_t width = 1;
    while (width < 8 && largest >> (8 * width)) {
        width *= 2;
    }
    return width;
}

static enum switch_prefs_result add_binary(const uint8_t *plist, size_t length, const char *key, int64_t value,
                                           uint8_t *out, size_t *out_length)
{
    struct bplist b;
    struct top_dict d;
    if (!read_binary(plist, length, &b, &d)) {
        return SWITCH_PREFS_UNREADABLE;
    }
    const size_t rs = b.ref_size, name = strlen(key);
    const uint8_t *keys = plist + d.at + d.header, *values = keys + (size_t)d.entries * rs;
    for (uint64_t i = 0; i < d.entries; ++i) {
        if (key_is(&b, be(keys + i * rs, rs), key)) {
            return SWITCH_PREFS_UNCHANGED;
        }
    }
    const uint64_t count = b.object_count, entries = d.entries + 1;
    if (rs < 8 && count + 3 > (uint64_t)1 << (8 * rs)) {
        return SWITCH_PREFS_UNREADABLE; /* the new objects' references would need wider ones throughout */
    }
    const size_t key_at = b.objects_end, value_at = key_at + put_marker(NULL, 0x50, name) + name;
    const size_t dict_at = value_at + put_integer(NULL, value);
    const size_t table = dict_at + put_marker(NULL, 0xd0, entries) + (size_t)entries * 2 * rs;
    const size_t os = offset_width(dict_at) > b.offset_size ? offset_width(dict_at) : b.offset_size;
    const size_t total = table + (size_t)(count + 3) * os + kTrailer;
    if (total > *out_length) {
        return SWITCH_PREFS_UNREADABLE;
    }
    memcpy(out, plist, b.objects_end);
    uint8_t *p = out + key_at;
    p += put_marker(p, 0x50, name);
    memcpy(p, key, name);
    put_integer(out + value_at, value);
    p = out + dict_at;
    p += put_marker(p, 0xd0, entries);
    memcpy(p, keys, (size_t)d.entries * rs);
    put_be(p + (size_t)d.entries * rs, rs, count);
    p += (size_t)entries * rs;
    memcpy(p, values, (size_t)d.entries * rs);
    put_be(p + (size_t)d.entries * rs, rs, count + 1);
    p = out + table;
    for (uint64_t i = 0; i < count; ++i) {
        put_be(p + i * os, os, be(b.offsets + i * b.offset_size, b.offset_size));
    }
    put_be(p + count * os, os, key_at);
    put_be(p + (count + 1) * os, os, value_at);
    put_be(p + (count + 2) * os, os, dict_at);
    uint8_t *trailer = p + (count + 3) * os;
    memcpy(trailer, plist + length - kTrailer, 6);
    trailer[6] = (uint8_t)os;
    trailer[7] = (uint8_t)rs;
    put_be(trailer + 8, 8, count + 3);
    put_be(trailer + 16, 8, count + 2);
    put_be(trailer + 24, 8, table);
    *out_length = total;
    return SWITCH_PREFS_ADDED;
}

static size_t put_text(uint8_t *p, const char *text)
{
    const size_t size = strlen(text);
    memcpy(p, text, size);
    return size;
}

/* The decimal digits of `value`, written at `p`; returns how many. */
static size_t put_decimal(uint8_t *p, int64_t value)
{
    char digits[21];
    size_t n = 0, size = 0;
    uint64_t magnitude = value < 0 ? 0 - (uint64_t)value : (uint64_t)value;
    do {
        digits[n++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (value < 0) {
        p[size++] = '-';
    }
    while (n) {
        p[size++] = (uint8_t)digits[--n];
    }
    return size;
}

static enum switch_prefs_result add_xml(const uint8_t *plist, size_t length, const char *key, int64_t value,
                                        uint8_t *out, size_t *out_length)
{
    const size_t name = strlen(key);
    for (size_t at = find(plist, length, 0, "<key>", 5); at < length; at = find(plist, length, at + 1, "<key>", 5)) {
        if (length - at >= name + 11 && !memcmp(plist + at + 5, key, name) &&
            !memcmp(plist + at + 5 + name, "</key>", 6)) {
            return SWITCH_PREFS_UNCHANGED;
        }
    }
    const size_t root = find(plist, length, 0, "<plist", 6);
    size_t open = find(plist, length, root, "<dict", 5), skip = 0;
    if (open < length && length - open >= 6 && !memcmp(plist + open, "<dict>", 6)) {
        skip = 6;
    } else if (open < length && length - open >= 7 && !memcmp(plist + open, "<dict/>", 7)) {
        skip = 7;
    } else {
        return SWITCH_PREFS_UNREADABLE;
    }
    if (length + name + 64 > *out_length) {
        return SWITCH_PREFS_UNREADABLE;
    }
    memcpy(out, plist, open);
    uint8_t *p = out + open;
    p += put_text(p, "<dict><key>");
    memcpy(p, key, name);
    p += name;
    p += put_text(p, "</key><integer>");
    p += put_decimal(p, value);
    p += put_text(p, "</integer>");
    if (skip == 7) {
        p += put_text(p, "</dict>");
    }
    memcpy(p, plist + open + skip, length - open - skip);
    *out_length = (size_t)(p - out) + length - open - skip;
    return SWITCH_PREFS_ADDED;
}

size_t switch_prefs_add_room(size_t length)
{
    return 10 * length + 512;
}

enum switch_prefs_result switch_prefs_add_integer(const uint8_t *plist, size_t length, const char *key, int64_t value,
                                                  uint8_t *out, size_t *out_length)
{
    if (!plist || !key || !out || !out_length || length > PREFS_ADD_MAX_LENGTH || !*key || strlen(key) > 255) {
        return SWITCH_PREFS_UNREADABLE;
    }
    if (length >= kMagic && !memcmp(plist, "bplist00", kMagic)) {
        return add_binary(plist, length, key, value, out, out_length);
    }
    const size_t head = length < 256 ? length : 256;
    if (find(plist, head, 0, "<plist", 6) < head) {
        return add_xml(plist, length, key, value, out, out_length);
    }
    return SWITCH_PREFS_UNREADABLE;
}
