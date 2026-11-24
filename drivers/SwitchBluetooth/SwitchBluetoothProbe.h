// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug builds only (SwitchBluetooth.cpp includes it under DEVELOPMENT || DEBUG): os_log lines that show, from one
// boot, how bluetoothd's LE scanning and advert filtering commands meet this controller. Every line starts
// "SwitchBluetooth: probe ":
// - cmd: each such command bluetoothd issues (opcode/sub-operation, length, first parameters) and what the driver did
//   with it: forwarded unchanged, forwarded rewritten (the bytes sent), answered here (the status it answered), or
//   dropped. A status line with no cmd line before it is a command that went out unchanged without passing
//   clientCommand (it arrived split across sends).
// - status: the controller's Command Complete or Status for each one forwarded, with its return parameters.
// - inject: each APCF command programLeFilter sends of its own, with the controller's status.
// - caps: the 0xFD53 reply at bring-up.
// - rule: each distinct 0xFCE9 matching rule once, as bluetoothd sent it, with the type and RSSI floor that
//   switch_le_filter_take reads at its fixed offsets.
#pragma once
#include <IOKit/IOLib.h>
#include <os/log.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "SwitchBluetoothLeFilter.h"
#include "SwitchCoex.h"

#define SWITCH_BT_PROBE_SHORT     24u  // parameter bytes on a cmd or status line
#define SWITCH_BT_PROBE_LONG      64u  // on a rule or caps line
#define SWITCH_BT_PROBE_RULES     32u  // distinct rules logged a boot
#define SWITCH_BT_PROBE_RULE_TYPE 52u  // switch_le_filter_take's rule layout: Apple continuity type, then RSSI floor
#define SWITCH_BT_PROBE_RULE_RSSI 53u

enum : uint16_t {
    SWITCH_BT_PROBE_OP_LE_SET_EVENT_MASK = 0x2001,
};

struct switch_bt_probe_command {
    uint16_t opcode;
    uint8_t length;
    bool wanted;
    uint8_t params[SWITCH_BT_PROBE_SHORT];  // as bluetoothd issued them, before any rewrite
};

struct switch_bt_probe {
    uint32_t rules[SWITCH_BT_PROBE_RULES];  // hashes of the rules logged (client thread)
    uint32_t ruleCount;
};

inline bool switch_bt_probe_wanted(uint16_t opcode)
{
    switch (opcode) {
    case SWITCH_BT_PROBE_OP_LE_SET_EVENT_MASK:
    case SWITCH_COEX_OP_LE_SCAN_PARAMS:
    case SWITCH_COEX_OP_LE_SCAN_ENABLE:
    case SWITCH_COEX_OP_LE_EXT_SCAN_PARAMS:
    case SWITCH_COEX_OP_LE_EXT_SCAN_ENABLE:
    case SWITCH_LE_OP_META:
    case SWITCH_LE_OP_OLYMPIC:
    case SWITCH_LE_OP_VENDOR_CAPS:
    case SWITCH_LE_OP_APCF:
        return true;
    default:
        return false;
    }
}

inline void switch_bt_probe_hex(char *out, size_t size, const uint8_t *data, uint32_t count)
{
    size_t at = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < count && at + 3 <= size; ++i)
        at += static_cast<size_t>(snprintf(out + at, size - at, "%02x", data[i]));
}

inline void switch_bt_probe_name(char *out, size_t size, uint16_t opcode, const uint8_t *params, uint32_t length)
{
    const bool sub = opcode == SWITCH_LE_OP_META || opcode == SWITCH_LE_OP_OLYMPIC || opcode == SWITCH_LE_OP_APCF;
    if (sub && length != 0u) {
        snprintf(out, size, "%04x/%02x", opcode, params[0]);
        return;
    }
    snprintf(out, size, "%04x", opcode);
}

inline uint32_t switch_bt_probe_hash(const uint8_t *bytes, uint8_t length)
{
    uint32_t hash = 2166136261u ^ length;  // FNV-1a
    for (uint32_t i = 0; i < length; ++i)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

inline void switch_bt_probe_rule(switch_bt_probe *probe, const uint8_t *params, uint8_t length)
{
    const uint32_t hash = switch_bt_probe_hash(params, length);
    for (uint32_t i = 0; i < probe->ruleCount; ++i) {
        if (probe->rules[i] == hash)
            return;
    }
    if (probe->ruleCount == SWITCH_BT_PROBE_RULES)
        return;
    probe->rules[probe->ruleCount++] = hash;
    char bytes[2 * SWITCH_BT_PROBE_LONG + 1];
    switch_bt_probe_hex(bytes, sizeof(bytes), params, length < SWITCH_BT_PROBE_LONG ? length : SWITCH_BT_PROBE_LONG);
    if (length <= SWITCH_BT_PROBE_RULE_RSSI) {
        os_log(OS_LOG_DEFAULT, "SwitchBluetooth: probe rule fce9/%02x len %u short [%s]", params[0], length, bytes);
        return;
    }
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: probe rule fce9/%02x len %u type@%u 0x%02x rssi@%u %d [%s]", params[0],
           length, SWITCH_BT_PROBE_RULE_TYPE, params[SWITCH_BT_PROBE_RULE_TYPE], SWITCH_BT_PROBE_RULE_RSSI,
           static_cast<int8_t>(params[SWITCH_BT_PROBE_RULE_RSSI]), bytes);
}

// Client thread, before the driver looks at the command: the copy the later lines describe it by.
inline switch_bt_probe_command switch_bt_probe_issue(switch_bt_probe *probe, uint16_t opcode, const uint8_t *params,
                                                     uint8_t length)
{
    switch_bt_probe_command c = {};
    c.opcode = opcode;
    c.length = length;
    c.wanted = switch_bt_probe_wanted(opcode);
    if (!c.wanted)
        return c;
    memcpy(c.params, params, length < SWITCH_BT_PROBE_SHORT ? length : SWITCH_BT_PROBE_SHORT);
    const bool rule = opcode == SWITCH_LE_OP_META && length != 0u &&
                      (params[0] == SWITCH_LE_META_RULE_ADD || params[0] == SWITCH_LE_META_RULE_REMOVE);
    if (rule)
        switch_bt_probe_rule(probe, params, length);
    return c;
}

inline void switch_bt_probe_describe(const switch_bt_probe_command *c, char *name, size_t nameSize, char *bytes,
                                     size_t bytesSize)
{
    switch_bt_probe_name(name, nameSize, c->opcode, c->params, c->length);
    switch_bt_probe_hex(bytes, bytesSize, c->params,
                        c->length < SWITCH_BT_PROBE_SHORT ? c->length : SWITCH_BT_PROBE_SHORT);
}

// event: the Command Complete answer() queued for it, H4 type first; length 5 carries a sub-operation.
inline void switch_bt_probe_answered(const switch_bt_probe_command *c, const uint8_t *event)
{
    if (!c->wanted)
        return;
    char name[8];
    char bytes[2 * SWITCH_BT_PROBE_SHORT + 1];
    switch_bt_probe_describe(c, name, sizeof(name), bytes, sizeof(bytes));
    if (event[2] > 4) {
        os_log(OS_LOG_DEFAULT, "SwitchBluetooth: probe cmd %s len %u [%s] -> answered here, status 0x%02x sub 0x%02x",
               name, c->length, bytes, event[6], event[7]);
        return;
    }
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: probe cmd %s len %u [%s] -> answered here, status 0x%02x", name, c->length,
           bytes, event[6]);
}

inline void switch_bt_probe_dropped(const switch_bt_probe_command *c)
{
    if (!c->wanted)
        return;
    char name[8];
    char bytes[2 * SWITCH_BT_PROBE_SHORT + 1];
    switch_bt_probe_describe(c, name, sizeof(name), bytes, sizeof(bytes));
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: probe cmd %s len %u [%s] -> dropped, no room to answer (ENOBUFS)", name,
           c->length, bytes);
}

// params: what goes to the controller.
inline void switch_bt_probe_forwarded(const switch_bt_probe_command *c, const uint8_t *params)
{
    if (!c->wanted)
        return;
    char name[8];
    char bytes[2 * SWITCH_BT_PROBE_SHORT + 1];
    switch_bt_probe_describe(c, name, sizeof(name), bytes, sizeof(bytes));
    const uint32_t shown = c->length < SWITCH_BT_PROBE_SHORT ? c->length : SWITCH_BT_PROBE_SHORT;
    if (memcmp(c->params, params, shown) == 0) {
        os_log(OS_LOG_DEFAULT, "SwitchBluetooth: probe cmd %s len %u [%s] -> forwarded unchanged", name, c->length,
               bytes);
        return;
    }
    char sent[2 * SWITCH_BT_PROBE_SHORT + 1];
    switch_bt_probe_hex(sent, sizeof(sent), params, shown);
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: probe cmd %s len %u [%s] -> forwarded rewritten [%s]", name, c->length,
           bytes, sent);
}

// Work loop: the controller's completion of one of the client's commands (status: a Command Status event), ret the
// return parameters after the status byte. vendor: the client's vendor command that frameRx takes it for when the
// controller put an LE opcode in the event instead, else 0.
inline void switch_bt_probe_completion(uint16_t opcode, uint16_t vendor, bool status, uint8_t result,
                                       const uint8_t *ret, uint32_t retLength)
{
    const uint16_t command = vendor != 0u ? vendor : opcode;
    if (!switch_bt_probe_wanted(command))
        return;
    char bytes[2 * SWITCH_BT_PROBE_SHORT + 1];
    const uint32_t shown = retLength < SWITCH_BT_PROBE_SHORT ? retLength : SWITCH_BT_PROBE_SHORT;
    switch_bt_probe_hex(bytes, sizeof(bytes), ret, shown);
    if (vendor != 0u) {
        os_log(OS_LOG_DEFAULT, "SwitchBluetooth: probe status %04x Command %s 0x%02x under opcode %04x [%s]", command,
               status ? "Status" : "Complete", result, opcode, bytes);
        return;
    }
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: probe status %04x Command %s 0x%02x [%s]", command,
           status ? "Status" : "Complete", result, bytes);
}

// status: the controller's, or inject()'s own 0xfe (not sent, a client command outstanding), 0xfd (not sent) and 0xff
// (no answer in time); ret holds nothing for those.
inline void switch_bt_probe_injected(const switch_le_command *c, uint8_t status, const uint8_t *ret, uint8_t retLength)
{
    char name[8];
    char bytes[2 * sizeof(switch_le_command::params) + 1];
    char returned[2 * SWITCH_BT_PROBE_SHORT + 1];
    switch_bt_probe_name(name, sizeof(name), c->opcode, c->params, c->length);
    switch_bt_probe_hex(bytes, sizeof(bytes), c->params, c->length);
    switch_bt_probe_hex(returned, sizeof(returned), ret, status < 0xfd ? retLength : 0u);
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: probe inject %s len %u [%s] -> status 0x%02x [%s]", name, c->length, bytes,
           status, returned);
}

// ret: the return parameters, status first.
inline void switch_bt_probe_capabilities(int error, const uint8_t *ret, uint8_t length)
{
    char bytes[2 * SWITCH_BT_PROBE_LONG + 1];
    switch_bt_probe_hex(bytes, sizeof(bytes), ret, length < SWITCH_BT_PROBE_LONG ? length : SWITCH_BT_PROBE_LONG);
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: probe caps fd53 error %d len %u [%s]", error, length, bytes);
}
