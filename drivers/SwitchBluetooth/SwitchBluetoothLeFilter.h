/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* bluetoothd's controller-side LE advert filtering, translated for this controller (SwitchBluetooth.cpp, BLE section).
 *
 * bluetoothd drives J120's BCM4355C0 filtering with Broadcom's BLE meta VSC 0xFCE9 (sub-operation first): 0x05 turns
 * the packet content filter's service-UUID condition on or off, 0x07/0x50/0x61 add, delete or clear a service UUID,
 * 0x32/0x33/0x1E add, remove or reset Apple's matching rules (Apple continuity type, RSSI floor, payload blob and
 * mask), 0x35 keeps an address list, 0x01 enables features; 0xFCE2 sub-operation 0x1E sets the duplicate-filter
 * bypass. CYW4356A3 answers the rules and 0xFCE2 with Unknown HCI Command. When the controller has Android's APCF
 * (0xFD53 LE Get Vendor Capabilities: filter_support, max_filter >= 3), the same state goes into three APCF filters
 * (0xFD57), which the controller ORs: index 1 the service UUIDs, index 2 every Apple advert (company 0x004C) at the
 * lowest rule floor, index 3 the addresses. Index 2 passes a superset: APCF matches manufacturer data only from its
 * start and an Apple advert carries several continuity entries, so bluetoothd sorts types on the host as it does with
 * no filtering. APCF stays disabled while bluetoothd filters nothing, so an unfiltered scan still sees every advert.
 *
 * Layouts: bluetoothd 21H461 bcm_vsc.c builders (r33-io RE, re/bt_le_filter.md); APCF from AOSP system/bt
 * stack/btm/btm_ble_adv_filter.c and btm_ble_api.h, the capability reply from Wireshark's
 * packet-bthci_vendor_broadcom.c. Plain C: the kernel driver and the host test (test/switch_bluetooth_le_filter_test)
 * include this one header. */
#ifndef SWITCH_BLUETOOTH_LE_FILTER_H
#define SWITCH_BLUETOOTH_LE_FILTER_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
#define SWITCH_LE_FN inline
#else
#define SWITCH_LE_FN static inline
#endif

enum {
    SWITCH_LE_OP_META = 0xfce9,
    SWITCH_LE_OP_OLYMPIC = 0xfce2,
    SWITCH_LE_OP_VENDOR_CAPS = 0xfd53,
    SWITCH_LE_OP_APCF = 0xfd57,
    SWITCH_LE_OP_SCAN_ENABLE = 0x200c,
};

enum {
    SWITCH_LE_META_ENABLE = 0x01,
    SWITCH_LE_META_PCF_SELECT = 0x05,
    SWITCH_LE_META_UUID = 0x07,
    SWITCH_LE_META_UUID_V2 = 0x50,
    SWITCH_LE_META_UUID_V3 = 0x61,
    SWITCH_LE_META_RULE_ADD = 0x32,
    SWITCH_LE_META_RULE_REMOVE = 0x33,
    SWITCH_LE_META_RULES_RESET = 0x1e,
    SWITCH_LE_META_ADDRESSES = 0x35,
    SWITCH_LE_OLYMPIC_DUP_BYPASS = 0x1e,
};

/* APCF sub-codes, actions and feature bits (btm_ble_api.h). */
enum {
    SWITCH_APCF_ENABLE = 0x00,
    SWITCH_APCF_FEATURE_SELECT = 0x01,
    SWITCH_APCF_ADDRESS = 0x02,
    SWITCH_APCF_UUID = 0x03,
    SWITCH_APCF_MANUFACTURER = 0x06,
    SWITCH_APCF_ADD = 0x00,
    SWITCH_APCF_DELETE = 0x01,
    SWITCH_APCF_CLEAR = 0x02,
    SWITCH_APCF_FEATURE_ADDRESS = 0x0001,
    SWITCH_APCF_FEATURE_UUID = 0x0004,
    SWITCH_APCF_FEATURE_MANUFACTURER = 0x0020,
    SWITCH_LE_INDEX_UUIDS = 1,
    SWITCH_LE_INDEX_APPLE = 2,
    SWITCH_LE_INDEX_ADDRESSES = 3,
};

#define SWITCH_LE_MAX_UUIDS     16u
#define SWITCH_LE_MAX_RULES     16u
#define SWITCH_LE_MAX_ADDRESSES 16u
#define SWITCH_LE_MAX_COMMANDS  (4u + SWITCH_LE_MAX_UUIDS + SWITCH_LE_MAX_ADDRESSES + 6u)
#define SWITCH_LE_RSSI_ANY      ((int8_t)-127)

typedef struct {
    uint16_t opcode;
    uint8_t length;
    uint8_t params[40];
} switch_le_command;

typedef struct {
    uint8_t length;   /* 2 or 16 */
    uint8_t uuid[16]; /* little endian, as bluetoothd sends it */
} switch_le_uuid;

typedef struct {
    uint8_t type;
    int8_t rssi;
} switch_le_rule;

typedef struct {
    uint8_t type;
    uint8_t address[6]; /* little endian */
} switch_le_address;

typedef struct {
    bool apcf;          /* the controller has APCF with at least three filters (switch_le_filter_capabilities) */
    bool uuidFiltering; /* bluetoothd's 0x05: the service-UUID condition is on */
    uint32_t features;  /* 0x01, kept as asked */
    uint32_t dupBypass; /* 0xFCE2/0x1E: nonzero lets duplicates through (switch_le_filter_scan_enable) */
    uint8_t uuidCount, ruleCount, addressCount;
    switch_le_uuid uuids[SWITCH_LE_MAX_UUIDS];
    switch_le_rule rules[SWITCH_LE_MAX_RULES];
    switch_le_address addresses[SWITCH_LE_MAX_ADDRESSES];
} switch_le_filter;

/* The 0xFD53 reply's return parameters (status first): APCF needs status 0, filter_support and max_filter >= 3. */
SWITCH_LE_FN bool switch_le_filter_capabilities(const uint8_t *ret, uint8_t length)
{
    return length >= 8 && ret[0] == 0 && ret[6] != 0 && ret[7] >= 3;
}

SWITCH_LE_FN void switch_le_command_init(switch_le_command *c, uint16_t opcode)
{
    c->opcode = opcode;
    c->length = 0;
}

SWITCH_LE_FN void switch_le_put(switch_le_command *c, const void *bytes, uint8_t length)
{
    if ((unsigned)c->length + length > sizeof(c->params))
        return;
    memcpy(c->params + c->length, bytes, length);
    c->length = (uint8_t)(c->length + length);
}

SWITCH_LE_FN void switch_le_put8(switch_le_command *c, uint8_t value)
{
    switch_le_put(c, &value, 1);
}

SWITCH_LE_FN void switch_le_put16(switch_le_command *c, uint16_t value)
{
    const uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    switch_le_put(c, bytes, 2);
}

/* APCF feature selection for one index: OR list logic, immediate delivery, the RSSI floor. */
SWITCH_LE_FN void switch_le_select(switch_le_command *c, uint8_t index, uint16_t features, int8_t rssi)
{
    switch_le_command_init(c, SWITCH_LE_OP_APCF);
    switch_le_put8(c, SWITCH_APCF_FEATURE_SELECT);
    switch_le_put8(c, SWITCH_APCF_ADD);
    switch_le_put8(c, index);
    switch_le_put16(c, features);
    switch_le_put16(c, 0); /* list logic: OR */
    switch_le_put8(c, 0);  /* filter logic */
    switch_le_put8(c, (uint8_t)rssi);
    switch_le_put8(c, 0); /* delivery mode: immediate */
}

SWITCH_LE_FN void switch_le_unselect(switch_le_command *c, uint8_t index)
{
    switch_le_command_init(c, SWITCH_LE_OP_APCF);
    switch_le_put8(c, SWITCH_APCF_FEATURE_SELECT);
    switch_le_put8(c, SWITCH_APCF_DELETE);
    switch_le_put8(c, index);
}

SWITCH_LE_FN void switch_le_clear(switch_le_command *c, uint8_t subcode, uint8_t index)
{
    switch_le_command_init(c, SWITCH_LE_OP_APCF);
    switch_le_put8(c, subcode);
    switch_le_put8(c, SWITCH_APCF_CLEAR);
    switch_le_put8(c, index);
}

SWITCH_LE_FN void switch_le_uuid_command(switch_le_command *c, uint8_t action, const switch_le_uuid *u)
{
    static const uint8_t kMask[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    switch_le_command_init(c, SWITCH_LE_OP_APCF);
    switch_le_put8(c, SWITCH_APCF_UUID);
    switch_le_put8(c, action);
    switch_le_put8(c, SWITCH_LE_INDEX_UUIDS);
    switch_le_put(c, u->uuid, u->length);
    switch_le_put(c, kMask, u->length);
}

SWITCH_LE_FN void switch_le_address_command(switch_le_command *c, uint8_t action, const switch_le_address *a)
{
    switch_le_command_init(c, SWITCH_LE_OP_APCF);
    switch_le_put8(c, SWITCH_APCF_ADDRESS);
    switch_le_put8(c, action);
    switch_le_put8(c, SWITCH_LE_INDEX_ADDRESSES);
    switch_le_put(c, a->address, 6);
    switch_le_put8(c, a->type);
}

/* Index 2's one entry: manufacturer data that starts with Apple's company identifier. */
SWITCH_LE_FN void switch_le_apple_command(switch_le_command *c)
{
    static const uint8_t kApple[] = {0x4c, 0x00, 0xff, 0xff};
    switch_le_command_init(c, SWITCH_LE_OP_APCF);
    switch_le_put8(c, SWITCH_APCF_MANUFACTURER);
    switch_le_put8(c, SWITCH_APCF_ADD);
    switch_le_put8(c, SWITCH_LE_INDEX_APPLE);
    switch_le_put(c, kApple, sizeof(kApple));
}

SWITCH_LE_FN void switch_le_enable_command(switch_le_command *c, bool on)
{
    switch_le_command_init(c, SWITCH_LE_OP_APCF);
    switch_le_put8(c, SWITCH_APCF_ENABLE);
    switch_le_put8(c, (uint8_t)(on ? 1 : 0));
}

SWITCH_LE_FN bool switch_le_filter_any(const switch_le_filter *f)
{
    return (f->uuidFiltering && f->uuidCount) || f->ruleCount || f->addressCount;
}

SWITCH_LE_FN int8_t switch_le_rules_floor(const switch_le_filter *f)
{
    int8_t floor = 127;
    for (unsigned i = 0; i < f->ruleCount; ++i) {
        if (f->rules[i].rssi < floor)
            floor = f->rules[i].rssi;
    }
    return floor;
}

/* The whole state as APCF commands, from a cleared controller: every index cleared, the entries, the selections, then
 * enable (or disable when nothing filters). Used after each change and after every bring-up. Returns the count. */
SWITCH_LE_FN unsigned switch_le_filter_program(const switch_le_filter *f, switch_le_command *out, unsigned max)
{
    unsigned n = 0;
#define SWITCH_LE_NEXT() (n < max ? &out[n++] : (switch_le_command *)0)
    switch_le_command *c;
    if ((c = SWITCH_LE_NEXT()))
        switch_le_enable_command(c, false);
    for (uint8_t index = SWITCH_LE_INDEX_UUIDS; index <= SWITCH_LE_INDEX_ADDRESSES; ++index) {
        if ((c = SWITCH_LE_NEXT()))
            switch_le_unselect(c, index);
    }
    if (!switch_le_filter_any(f))
        return n;
    if (f->uuidFiltering && f->uuidCount) {
        for (unsigned i = 0; i < f->uuidCount; ++i) {
            if ((c = SWITCH_LE_NEXT()))
                switch_le_uuid_command(c, SWITCH_APCF_ADD, &f->uuids[i]);
        }
        if ((c = SWITCH_LE_NEXT()))
            switch_le_select(c, SWITCH_LE_INDEX_UUIDS, SWITCH_APCF_FEATURE_UUID, SWITCH_LE_RSSI_ANY);
    }
    if (f->ruleCount) {
        if ((c = SWITCH_LE_NEXT()))
            switch_le_apple_command(c);
        if ((c = SWITCH_LE_NEXT()))
            switch_le_select(c, SWITCH_LE_INDEX_APPLE, SWITCH_APCF_FEATURE_MANUFACTURER, switch_le_rules_floor(f));
    }
    if (f->addressCount) {
        for (unsigned i = 0; i < f->addressCount; ++i) {
            if ((c = SWITCH_LE_NEXT()))
                switch_le_address_command(c, SWITCH_APCF_ADD, &f->addresses[i]);
        }
        if ((c = SWITCH_LE_NEXT()))
            switch_le_select(c, SWITCH_LE_INDEX_ADDRESSES, SWITCH_APCF_FEATURE_ADDRESS, SWITCH_LE_RSSI_ANY);
    }
    if ((c = SWITCH_LE_NEXT()))
        switch_le_enable_command(c, true);
#undef SWITCH_LE_NEXT
    return n;
}

SWITCH_LE_FN void switch_le_uuid_update(switch_le_filter *f, uint8_t action, const uint8_t *uuid, uint8_t length)
{
    if (action == SWITCH_APCF_CLEAR) {
        f->uuidCount = 0;
        return;
    }
    if (length != 2 && length != 16)
        return;
    unsigned at = 0;
    while (at < f->uuidCount && !(f->uuids[at].length == length && !memcmp(f->uuids[at].uuid, uuid, length)))
        ++at;
    if (action == SWITCH_APCF_ADD && at == f->uuidCount && f->uuidCount < SWITCH_LE_MAX_UUIDS) {
        f->uuids[f->uuidCount].length = length;
        memcpy(f->uuids[f->uuidCount].uuid, uuid, length);
        ++f->uuidCount;
    } else if (action == SWITCH_APCF_DELETE && at < f->uuidCount) {
        f->uuids[at] = f->uuids[--f->uuidCount];
    }
}

SWITCH_LE_FN void switch_le_rule_update(switch_le_filter *f, bool add, uint8_t type, int8_t rssi)
{
    unsigned at = 0;
    while (at < f->ruleCount && f->rules[at].type != type)
        ++at;
    if (add && at == f->ruleCount && f->ruleCount < SWITCH_LE_MAX_RULES)
        ++f->ruleCount;
    if (add && at < f->ruleCount) {
        f->rules[at].type = type;
        f->rules[at].rssi = rssi;
    } else if (!add && at < f->ruleCount) {
        f->rules[at] = f->rules[--f->ruleCount];
    }
}

SWITCH_LE_FN void switch_le_address_update(switch_le_filter *f, uint8_t action, uint8_t type, const uint8_t *address)
{
    if (action == SWITCH_APCF_CLEAR) {
        f->addressCount = 0;
        return;
    }
    unsigned at = 0;
    while (at < f->addressCount && memcmp(f->addresses[at].address, address, 6))
        ++at;
    if (action == SWITCH_APCF_ADD && at == f->addressCount && f->addressCount < SWITCH_LE_MAX_ADDRESSES) {
        f->addresses[f->addressCount].type = type;
        memcpy(f->addresses[f->addressCount].address, address, 6);
        ++f->addressCount;
    } else if (action == SWITCH_APCF_DELETE && at < f->addressCount) {
        f->addresses[at] = f->addresses[--f->addressCount];
    }
}

/* One of bluetoothd's commands. True when it is a filtering command this header translates: the state takes it and
 * *sub is the sub-operation the synthetic Command Complete echoes ([00][sub]); the caller then programs the
 * controller (switch_le_filter_program). False leaves the command to the controller. */
SWITCH_LE_FN bool switch_le_filter_take(switch_le_filter *f, uint16_t opcode, const uint8_t *p, uint8_t length,
                                        uint8_t *sub)
{
    if (!f->apcf || !length)
        return false;
    *sub = p[0];
    if (opcode == SWITCH_LE_OP_OLYMPIC) {
        if (p[0] != SWITCH_LE_OLYMPIC_DUP_BYPASS || length < 8)
            return false;
        f->dupBypass = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        return true;
    }
    if (opcode != SWITCH_LE_OP_META)
        return false;
    switch (p[0]) {
    case SWITCH_LE_META_ENABLE:
        if (length < 5)
            return false;
        f->features = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
        return true;
    case SWITCH_LE_META_PCF_SELECT:
        if (length < 2)
            return false;
        f->uuidFiltering = p[1] == SWITCH_APCF_ADD;
        return true;
    case SWITCH_LE_META_UUID: /* [07][action][6 x 00][02][00][UUID] */
        if (length < 9)
            return false;
        switch_le_uuid_update(f, p[1], p + 10, (uint8_t)(length > 10 ? length - 10 : 0));
        return true;
    case SWITCH_LE_META_UUID_V2: /* [50][action][6 x 00][02][00][b][c][d][UUID] */
        if (length < 9)
            return false;
        switch_le_uuid_update(f, p[1], p + 13, (uint8_t)(length > 13 ? length - 13 : 0));
        return true;
    case SWITCH_LE_META_RULE_ADD:
    case SWITCH_LE_META_RULE_REMOVE: /* [op][flag][6 x 00][blob 22][mask 22][type][rssi] */
        if (length < 54)
            return false;
        switch_le_rule_update(f, p[0] == SWITCH_LE_META_RULE_ADD, p[52], (int8_t)p[53]);
        return true;
    case SWITCH_LE_META_RULES_RESET:
        f->ruleCount = 0;
        return true;
    case SWITCH_LE_META_ADDRESSES: /* add [35][00][type][addr 6], remove [35][01][type][addr 6], clear [35][02] */
        if (length < 2 || (p[1] != SWITCH_APCF_CLEAR && length < 9))
            return false;
        switch_le_address_update(f, p[1], (uint8_t)(length >= 9 ? p[2] : 0), p + 3);
        return true;
    default:
        return false;
    }
}

/* LE Set Scan Enable {enable, filter duplicates}: duplicates pass while bluetoothd asked for the bypass. True if it
 * changed the command. */
SWITCH_LE_FN bool switch_le_filter_scan_enable(const switch_le_filter *f, uint8_t *params, uint8_t length)
{
    if (!f->apcf || !f->dupBypass || length < 2 || !params[1])
        return false;
    params[1] = 0;
    return true;
}

#undef SWITCH_LE_FN
#endif
