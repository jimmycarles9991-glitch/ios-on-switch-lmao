// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchBluetooth's view of the controller's links (SwitchBluetooth.cpp, "Links"): a per-handle table of ACL packet
// counts and the one-line descriptions of the link commands and events it traces. Plain C on byte buffers, so
// test/switch_bluetooth_links_test.cpp drives it on the host with captured HCI. The includer declares snprintf
// (IOLib.h in the kernel, stdio.h on the host).
//
// Credits. The host may have at most the controller's HCI_Total_Num_ACL_Data_Packets (Read Buffer Size) ACL packets
// in the controller at a time, shared by every link; each HCI_Number_Of_Completed_Packets gives some back, per handle.
// The table counts per handle the packets the client sent (tx), the controller's completions (done) and the most
// that were ever outstanding (peak = max tx - done): a link whose tx - done only grows is one whose credits are lost,
// and the sum over links of tx - done is what the controller holds.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_BT_LINKS 8  // more than the 7 active ACL links a BR/EDR piconet allows, plus LE

enum {
    SWITCH_BT_LINK_FREE = 0,
    SWITCH_BT_LINK_ACL = 1,
    SWITCH_BT_LINK_LE = 2
};

enum {
    SWITCH_BT_ROLE_UNKNOWN = 0xff
};

struct switch_bt_link {
    uint16_t handle;
    uint8_t kind;       // SWITCH_BT_LINK_*
    uint8_t role;       // HCI role: 0 central (master), 1 peripheral (slave); SWITCH_BT_ROLE_UNKNOWN
    uint8_t addr[6];    // as HCI carries it (least significant byte first)
    uint8_t mode;       // Mode Change: 0 active, 1 hold, 2 sniff
    uint8_t encrypted;  // Encryption Change: 1 on
    uint16_t interval;  // Mode Change: the sniff interval in 0.625 ms slots
    uint32_t rx;        // ACL packets from the controller
    uint32_t tx;        // ACL packets to it
    uint32_t done;      // completed (Number Of Completed Packets)
    uint32_t peak;      // the most tx - done ever was
    uint32_t window;    // the most tx - done since the coexistence snapshot last looked (SwitchBluetooth.cpp)
};

struct switch_bt_links {
    struct switch_bt_link link[SWITCH_BT_LINKS];
    uint32_t strayTx, strayDone;  // packets and completions for a handle the table does not hold
};

static inline uint16_t switch_bt_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (unsigned)p[1] << 8);
}

// The connection handle of an ACL or LE ACL header (handle in bits 11:0, packet boundary and broadcast flags above).
static inline uint16_t switch_bt_acl_handle(const uint8_t header[2])
{
    return (uint16_t)(switch_bt_le16(header) & 0x0fffu);
}

static inline struct switch_bt_link *switch_bt_link_find(struct switch_bt_links *links, uint16_t handle)
{
    // NOLINTNEXTLINE(modernize-loop-convert): C too (the host harness)
    for (unsigned i = 0; i < SWITCH_BT_LINKS; ++i) {
        if (links->link[i].kind != SWITCH_BT_LINK_FREE && links->link[i].handle == handle)
            return &links->link[i];
    }
    return NULL;
}

// A link the controller reported up (Connection Complete, LE Connection Complete): its slot, new or reused for the
// handle; NULL with the table full (its packets then count as strays).
static inline struct switch_bt_link *switch_bt_link_open(struct switch_bt_links *links, uint16_t handle, uint8_t kind,
                                                         const uint8_t addr[6], uint8_t role)
{
    struct switch_bt_link *slot = switch_bt_link_find(links, handle);
    for (unsigned i = 0; !slot && i < SWITCH_BT_LINKS; ++i) {
        if (links->link[i].kind == SWITCH_BT_LINK_FREE)
            slot = &links->link[i];
    }
    if (!slot)
        return NULL;
    const struct switch_bt_link fresh = {handle, kind, role, {0}, 0, 0, 0, 0, 0, 0, 0, 0};
    *slot = fresh;
    for (unsigned i = 0; i < 6; ++i)
        slot->addr[i] = addr[i];
    return slot;
}

// Disconnection Complete: the link's last state into *last (when given) and its slot freed. False if not held.
static inline bool switch_bt_link_close(struct switch_bt_links *links, uint16_t handle, struct switch_bt_link *last)
{
    struct switch_bt_link *slot = switch_bt_link_find(links, handle);
    if (!slot)
        return false;
    if (last)
        *last = *slot;
    slot->kind = SWITCH_BT_LINK_FREE;
    return true;
}

static inline void switch_bt_link_rx(struct switch_bt_links *links, uint16_t handle)
{
    struct switch_bt_link *slot = switch_bt_link_find(links, handle);
    if (slot)
        ++slot->rx;
}

static inline void switch_bt_link_tx(struct switch_bt_links *links, uint16_t handle)
{
    struct switch_bt_link *slot = switch_bt_link_find(links, handle);
    if (!slot) {
        ++links->strayTx;
        return;
    }
    ++slot->tx;
    if (slot->tx - slot->done > slot->peak)
        slot->peak = slot->tx - slot->done;
    if (slot->tx - slot->done > slot->window)
        slot->window = slot->tx - slot->done;
}

// HCI_Number_Of_Completed_Packets parameters (Num_Handles, then handle and count pairs): the counts added to their
// links. Returns the packets completed, or -1 for parameters shorter than they say.
static inline int switch_bt_links_completed(struct switch_bt_links *links, const uint8_t *params, uint32_t length)
{
    if (length < 1 || length < 1u + 4u * params[0])
        return -1;
    int total = 0;
    for (size_t i = 0; i < params[0]; ++i) {
        const uint16_t handle = (uint16_t)(switch_bt_le16(params + 1 + 4 * i) & 0x0fffu);
        const uint16_t count = switch_bt_le16(params + 3 + 4 * i);
        struct switch_bt_link *slot = switch_bt_link_find(links, handle);
        if (slot) {
            slot->done += count;
        } else {
            links->strayDone += count;
        }
        total += count;
    }
    return total;
}

// ---- Incoming gamepads: accepted as central ---------------------------------------------------------------------
//
// A Joy-Con reconnecting pages its host, so it is the link's central (master) at first. bluetoothd accepts with Role 1
// (stay peripheral) and leaves the switch to the Joy-Con, which asks for it ~170-290 ms after Connection Complete and
// only then authenticates and opens HID (r7v 11-05 12:34). The left one always went on from there; the right one
// often did not: after its switch it went quiet and disconnected 160 ms later (reason 0x13, 12:34:01.061 and 03.291),
// or it never switched and never authenticated until bluetoothd gave up 4 s later (05.591, 11.196, 16.080, 42.740),
// ignoring the SDP requests sent meanwhile (rx 0, tx 4 at 16.080). That this switch is the cause is a hypothesis
// (the left Joy-Con went through the same sequence fine); taking it out of the picture is the test. A console ends
// up central of its Joy-Cons, and a host that becomes central at the accept has no role switch left for the Joy-Con
// to make. So bluetoothd's Accept Connection Request for a gamepad or joystick (Class of Device major class
// Peripheral, minor 1 or 2, as its Connection Request said) goes out with Role 0: the controller switches roles
// before Connection Complete (a Role Change event first), and the host is central of every Joy-Con and of the
// AirPods it connects itself, with no scatternet. A remote that refuses the switch keeps its role and the connection
// completes as before.

#define SWITCH_BT_REQUESTS 4

struct switch_bt_request {
    uint8_t addr[6];  // HCI byte order
    uint8_t used;
    uint32_t cod;  // Class of Device
};

struct switch_bt_requests {
    struct switch_bt_request entry[SWITCH_BT_REQUESTS];
    uint32_t next;
};

// A gamepad or joystick: major device class Peripheral (5), minor bits 5:2 joystick (1) or gamepad (2).
static inline bool switch_bt_cod_is_gamepad(uint32_t cod)
{
    const uint32_t minor = (cod >> 2) & 0x0f;
    return ((cod >> 8) & 0x1f) == 0x05 && (minor == 1 || minor == 2);
}

// Connection Request (address, class): kept until its Accept Connection Request, the oldest entry replaced.
static inline void switch_bt_request_note(struct switch_bt_requests *requests, const uint8_t addr[6], uint32_t cod)
{
    struct switch_bt_request *slot = NULL;
    for (unsigned i = 0; i < SWITCH_BT_REQUESTS && !slot; ++i) {
        struct switch_bt_request *e = &requests->entry[i];
        bool same = e->used != 0;
        for (unsigned b = 0; b < 6 && same; ++b)
            same = e->addr[b] == addr[b];
        if (same)
            slot = e;
    }
    if (!slot)
        slot = &requests->entry[requests->next++ % SWITCH_BT_REQUESTS];
    for (unsigned b = 0; b < 6; ++b)
        slot->addr[b] = addr[b];
    slot->cod = cod;
    slot->used = 1;
}

// Accept Connection Request parameters (address, role): for a gamepad's request still held, a Role 1 becomes 0 and
// the request is consumed. Returns true when it changed the role; *cod gets the class when it did.
static inline bool switch_bt_accept_as_central(struct switch_bt_requests *requests, uint8_t *params, uint32_t length,
                                               uint32_t *cod)
{
    if (length < 7)
        return false;
    // NOLINTNEXTLINE(modernize-loop-convert): C too (the host harness)
    for (unsigned i = 0; i < SWITCH_BT_REQUESTS; ++i) {
        struct switch_bt_request *e = &requests->entry[i];
        bool same = e->used != 0;
        for (unsigned b = 0; b < 6 && same; ++b)
            same = e->addr[b] == params[b];
        if (!same)
            continue;
        e->used = 0;
        if (params[6] != 1 || !switch_bt_cod_is_gamepad(e->cod))
            return false;
        params[6] = 0;
        if (cod)
            *cod = e->cod;
        return true;
    }
    return false;
}

// ---- Tracing ------------------------------------------------------------------------------------------------------

enum {
    SWITCH_BT_EV_CONNECTION_COMPLETE = 0x03,
    SWITCH_BT_EV_CONNECTION_REQUEST = 0x04,
    SWITCH_BT_EV_DISCONNECTION_COMPLETE = 0x05,
    SWITCH_BT_EV_AUTHENTICATION_COMPLETE = 0x06,
    SWITCH_BT_EV_ENCRYPTION_CHANGE = 0x08,
    SWITCH_BT_EV_HARDWARE_ERROR = 0x10,
    SWITCH_BT_EV_ROLE_CHANGE = 0x12,
    SWITCH_BT_EV_NUM_COMPLETED = 0x13,
    SWITCH_BT_EV_MODE_CHANGE = 0x14,
    SWITCH_BT_EV_PIN_CODE_REQUEST = 0x16,
    SWITCH_BT_EV_LINK_KEY_REQUEST = 0x17,
    SWITCH_BT_EV_LINK_KEY_NOTIFICATION = 0x18,
    SWITCH_BT_EV_DATA_BUFFER_OVERFLOW = 0x1a,
    SWITCH_BT_EV_MAX_SLOTS_CHANGE = 0x1b,
    SWITCH_BT_EV_SNIFF_SUBRATING = 0x2e,
    SWITCH_BT_EV_IO_CAPABILITY_REQUEST = 0x31,
    SWITCH_BT_EV_IO_CAPABILITY_RESPONSE = 0x32,
    SWITCH_BT_EV_USER_CONFIRMATION_REQUEST = 0x33,
    SWITCH_BT_EV_SIMPLE_PAIRING_COMPLETE = 0x36,
    SWITCH_BT_EV_SUPERVISION_TIMEOUT_CHANGED = 0x38,
    SWITCH_BT_EV_LE_META = 0x3e,
    SWITCH_BT_LE_CONNECTION_COMPLETE = 0x01,
    SWITCH_BT_LE_ENHANCED_CONNECTION_COMPLETE = 0x0a,
};

struct switch_bt_name {
    uint16_t code;
    const char *name;
};

// The link events traced, each in full (they are short); LE meta only for its connection completes.
static const struct switch_bt_name switch_bt_link_events[] = {
    {SWITCH_BT_EV_CONNECTION_COMPLETE, "Connection Complete"},
    {SWITCH_BT_EV_CONNECTION_REQUEST, "Connection Request"},
    {SWITCH_BT_EV_DISCONNECTION_COMPLETE, "Disconnection Complete"},
    {SWITCH_BT_EV_AUTHENTICATION_COMPLETE, "Authentication Complete"},
    {SWITCH_BT_EV_ENCRYPTION_CHANGE, "Encryption Change"},
    {SWITCH_BT_EV_HARDWARE_ERROR, "Hardware Error"},
    {SWITCH_BT_EV_ROLE_CHANGE, "Role Change"},
    {SWITCH_BT_EV_MODE_CHANGE, "Mode Change"},
    {SWITCH_BT_EV_PIN_CODE_REQUEST, "PIN Code Request"},
    {SWITCH_BT_EV_LINK_KEY_REQUEST, "Link Key Request"},
    {SWITCH_BT_EV_LINK_KEY_NOTIFICATION, "Link Key Notification"},
    {SWITCH_BT_EV_DATA_BUFFER_OVERFLOW, "Data Buffer Overflow"},
    {SWITCH_BT_EV_MAX_SLOTS_CHANGE, "Max Slots Change"},
    {SWITCH_BT_EV_SNIFF_SUBRATING, "Sniff Subrating"},
    {SWITCH_BT_EV_IO_CAPABILITY_REQUEST, "IO Capability Request"},
    {SWITCH_BT_EV_IO_CAPABILITY_RESPONSE, "IO Capability Response"},
    {SWITCH_BT_EV_USER_CONFIRMATION_REQUEST, "User Confirmation Request"},
    {SWITCH_BT_EV_SIMPLE_PAIRING_COMPLETE, "Simple Pairing Complete"},
    {SWITCH_BT_EV_SUPERVISION_TIMEOUT_CHANGED, "Link Supervision Timeout Changed"},
};

// The commands traced at every use, with their Command Status or Complete: what makes, accepts, secures, shapes and
// ends a link, and the scan and policy settings that decide whether a second one can come up at all.
static const struct switch_bt_name switch_bt_link_commands[] = {
    {0x0405, "Create Connection"},
    {0x0406, "Disconnect"},
    {0x0408, "Create Connection Cancel"},
    {0x0409, "Accept Connection Request"},
    {0x040a, "Reject Connection Request"},
    {0x040b, "Link Key Request Reply"},
    {0x040c, "Link Key Request Negative Reply"},
    {0x0411, "Authentication Requested"},
    {0x0413, "Set Connection Encryption"},
    {0x042b, "IO Capability Request Reply"},
    {0x042c, "User Confirmation Request Reply"},
    {0x042d, "User Confirmation Request Negative Reply"},
    {0x0434, "IO Capability Request Negative Reply"},
    {0x0803, "Sniff Mode"},
    {0x0804, "Exit Sniff Mode"},
    {0x080b, "Switch Role"},
    {0x080d, "Write Link Policy Settings"},
    {0x080f, "Write Default Link Policy Settings"},
    {0x0811, "Sniff Subrating"},
    {0x0c05, "Set Event Filter"},
    {0x0c1a, "Write Scan Enable"},
    {0x0c1c, "Write Page Scan Activity"},
    {0x0c37, "Write Link Supervision Timeout"},
};

static inline const char *switch_bt_lookup(const struct switch_bt_name *names, size_t count, uint16_t code)
{
    for (size_t i = 0; i < count; ++i) {
        if (names[i].code == code)
            return names[i].name;
    }
    return NULL;
}

static inline const char *switch_bt_link_event_name(uint8_t code)
{
    return switch_bt_lookup(switch_bt_link_events, sizeof(switch_bt_link_events) / sizeof(switch_bt_link_events[0]),
                            code);
}

static inline const char *switch_bt_link_command_name(uint16_t opcode)
{
    return switch_bt_lookup(switch_bt_link_commands,
                            sizeof(switch_bt_link_commands) / sizeof(switch_bt_link_commands[0]), opcode);
}

// Whether an event is traced (and so taken whole before it is passed on): a link event, Number Of Completed Packets
// (counted), or an LE connection complete (`sub` is the LE meta sub-event, 0 if not known yet).
static inline bool switch_bt_event_wanted(uint8_t code, uint8_t sub)
{
    if (code == SWITCH_BT_EV_LE_META)
        return sub == SWITCH_BT_LE_CONNECTION_COMPLETE || sub == SWITCH_BT_LE_ENHANCED_CONNECTION_COMPLETE;
    return code == SWITCH_BT_EV_NUM_COMPLETED || switch_bt_link_event_name(code) != NULL;
}

// "48:f1:eb:b6:9e:d1" from HCI's byte order (least significant first); 18 bytes with the NUL.
static inline void switch_bt_format_addr(char out[18], const uint8_t *addr)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 6; ++i) {
        out[i * 3] = digits[addr[5 - i] >> 4];
        out[i * 3 + 1] = digits[addr[5 - i] & 15];
        out[i * 3 + 2] = i < 5 ? ':' : '\0';
    }
}

// One line for a traced link event (`params` are the event's parameters, `length` of them), without the driver's
// prefix; a too short event says so. Never prints a key.
static inline void switch_bt_describe_event(char *out, size_t size, uint8_t code, const uint8_t *params,
                                            uint32_t length)
{
    char a[18];
    const char *name = switch_bt_link_event_name(code);
    if (code == SWITCH_BT_EV_LE_META && length >= 1) {
        const uint8_t sub = params[0];
        const uint32_t need = sub == SWITCH_BT_LE_CONNECTION_COMPLETE ? 19u : 31u;
        if (length < need) {
            snprintf(out, size, "LE Connection Complete (sub-event 0x%02x) of %u bytes, too short", sub, length);
            return;
        }
        // status, handle, role, peer address type, peer address; the enhanced form has two RPAs before the interval.
        const uint8_t *timing = params + (sub == SWITCH_BT_LE_CONNECTION_COMPLETE ? 12 : 24);
        switch_bt_format_addr(a, params + 6);
        snprintf(out, size,
                 "LE Connection Complete%s %04x %s (type %u) status 0x%02x role %u interval %u latency %u timeout %u",
                 sub == SWITCH_BT_LE_CONNECTION_COMPLETE ? "" : " (enhanced)", switch_bt_le16(params + 2) & 0x0fffu, a,
                 params[5], params[1], params[4], switch_bt_le16(timing), switch_bt_le16(timing + 2),
                 switch_bt_le16(timing + 4));
        return;
    }
    if (!name)
        name = "event";
    uint32_t need = 0;
    switch (code) {
    case SWITCH_BT_EV_CONNECTION_COMPLETE:
        need = 11;
        break;
    case SWITCH_BT_EV_CONNECTION_REQUEST:
        need = 10;
        break;
    case SWITCH_BT_EV_DISCONNECTION_COMPLETE:
    case SWITCH_BT_EV_ENCRYPTION_CHANGE:
        need = 4;
        break;
    case SWITCH_BT_EV_AUTHENTICATION_COMPLETE:
    case SWITCH_BT_EV_MAX_SLOTS_CHANGE:
        need = 3;
        break;
    case SWITCH_BT_EV_HARDWARE_ERROR:
    case SWITCH_BT_EV_DATA_BUFFER_OVERFLOW:
        need = 1;
        break;
    case SWITCH_BT_EV_ROLE_CHANGE:
        need = 8;
        break;
    case SWITCH_BT_EV_MODE_CHANGE:
        need = 6;
        break;
    case SWITCH_BT_EV_LINK_KEY_NOTIFICATION:
        need = 23;
        break;
    case SWITCH_BT_EV_SNIFF_SUBRATING:
        need = 11;
        break;
    case SWITCH_BT_EV_IO_CAPABILITY_RESPONSE:
        need = 9;
        break;
    case SWITCH_BT_EV_USER_CONFIRMATION_REQUEST:
        need = 10;
        break;
    case SWITCH_BT_EV_SIMPLE_PAIRING_COMPLETE:
        need = 7;
        break;
    case SWITCH_BT_EV_SUPERVISION_TIMEOUT_CHANGED:
        need = 4;
        break;
    default:
        need = 6;
        break;  // the requests: an address
    }
    if (length < need) {
        snprintf(out, size, "%s (0x%02x) of %u bytes, too short", name, code, length);
        return;
    }
    switch (code) {
    case SWITCH_BT_EV_CONNECTION_COMPLETE:
        switch_bt_format_addr(a, params + 3);
        snprintf(out, size, "%s %04x %s status 0x%02x type %u encryption %u", name, switch_bt_le16(params + 1), a,
                 params[0], params[9], params[10]);
        break;
    case SWITCH_BT_EV_CONNECTION_REQUEST:
        switch_bt_format_addr(a, params);
        snprintf(out, size, "%s %s class %02x%02x%02x type %u", name, a, params[8], params[7], params[6], params[9]);
        break;
    case SWITCH_BT_EV_DISCONNECTION_COMPLETE:
        snprintf(out, size, "%s %04x status 0x%02x reason 0x%02x", name, switch_bt_le16(params + 1), params[0],
                 params[3]);
        break;
    case SWITCH_BT_EV_AUTHENTICATION_COMPLETE:
        snprintf(out, size, "%s %04x status 0x%02x", name, switch_bt_le16(params + 1), params[0]);
        break;
    case SWITCH_BT_EV_ENCRYPTION_CHANGE:
        snprintf(out, size, "%s %04x status 0x%02x on %u", name, switch_bt_le16(params + 1), params[0], params[3]);
        break;
    case SWITCH_BT_EV_HARDWARE_ERROR:
    case SWITCH_BT_EV_DATA_BUFFER_OVERFLOW:
        snprintf(out, size, "%s 0x%02x", name, params[0]);
        break;
    case SWITCH_BT_EV_ROLE_CHANGE:
        switch_bt_format_addr(a, params + 1);
        snprintf(out, size, "%s %s status 0x%02x role %u", name, a, params[0], params[7]);
        break;
    case SWITCH_BT_EV_MODE_CHANGE:
        snprintf(out, size, "%s %04x status 0x%02x mode %u interval %u", name, switch_bt_le16(params + 1), params[0],
                 params[3], switch_bt_le16(params + 4));
        break;
    case SWITCH_BT_EV_LINK_KEY_NOTIFICATION:
        switch_bt_format_addr(a, params);
        snprintf(out, size, "%s %s key type %u", name, a, params[22]);
        break;
    case SWITCH_BT_EV_MAX_SLOTS_CHANGE:
        snprintf(out, size, "%s %04x slots %u", name, switch_bt_le16(params), params[2]);
        break;
    case SWITCH_BT_EV_SNIFF_SUBRATING:
        snprintf(out, size, "%s %04x status 0x%02x tx latency %u rx latency %u", name, switch_bt_le16(params + 1),
                 params[0], switch_bt_le16(params + 3), switch_bt_le16(params + 5));
        break;
    case SWITCH_BT_EV_IO_CAPABILITY_RESPONSE:
        switch_bt_format_addr(a, params);
        snprintf(out, size, "%s %s capability %u oob %u authentication 0x%02x", name, a, params[6], params[7],
                 params[8]);
        break;
    case SWITCH_BT_EV_SIMPLE_PAIRING_COMPLETE:
        switch_bt_format_addr(a, params + 1);
        snprintf(out, size, "%s %s status 0x%02x", name, a, params[0]);
        break;
    case SWITCH_BT_EV_SUPERVISION_TIMEOUT_CHANGED:
        snprintf(out, size, "%s %04x timeout %u", name, switch_bt_le16(params), switch_bt_le16(params + 2));
        break;
    default:  // PIN Code, Link Key, IO Capability and User Confirmation Requests: the address
        switch_bt_format_addr(a, params);
        snprintf(out, size, "%s %s", name, a);
        break;
    }
}

// One line for a traced link command (`params` its parameters), without the driver's prefix. Link Key Request Reply
// carries the key: only its address is printed.
static inline void switch_bt_describe_command(char *out, size_t size, uint16_t opcode, const uint8_t *params,
                                              uint32_t length)
{
    char a[18];
    const char *name = switch_bt_link_command_name(opcode);
    if (!name)
        name = "command";
    switch (opcode) {
    case 0x0405:  // address, packet types, page scan repetition, reserved, clock offset, allow role switch
        if (length < 13)
            break;
        switch_bt_format_addr(a, params);
        snprintf(out, size, "%s %s packet types 0x%04x allow role switch %u", name, a, switch_bt_le16(params + 6),
                 params[12]);
        return;
    case 0x0406:
        if (length < 3)
            break;
        snprintf(out, size, "%s %04x reason 0x%02x", name, switch_bt_le16(params), params[2]);
        return;
    case 0x0409:  // role 0: become central (the controller switches roles), 1: stay peripheral
    case 0x040a:  // reason
    case 0x080b:  // role
        if (length < 7)
            break;
        switch_bt_format_addr(a, params);
        snprintf(out, size, "%s %s %s %u", name, a, opcode == 0x040a ? "reason" : "role", params[6]);
        return;
    case 0x0408:
    case 0x040b:
    case 0x040c:
    case 0x042c:
    case 0x042d:
        if (length < 6)
            break;
        switch_bt_format_addr(a, params);
        snprintf(out, size, "%s %s", name, a);
        return;
    case 0x042b:
        if (length < 9)
            break;
        switch_bt_format_addr(a, params);
        snprintf(out, size, "%s %s capability %u oob %u authentication 0x%02x", name, a, params[6], params[7],
                 params[8]);
        return;
    case 0x0434:
        if (length < 7)
            break;
        switch_bt_format_addr(a, params);
        snprintf(out, size, "%s %s reason 0x%02x", name, a, params[6]);
        return;
    case 0x0411:
    case 0x0804:
        if (length < 2)
            break;
        snprintf(out, size, "%s %04x", name, switch_bt_le16(params));
        return;
    case 0x0413:
        if (length < 3)
            break;
        snprintf(out, size, "%s %04x on %u", name, switch_bt_le16(params), params[2]);
        return;
    case 0x0803:
        if (length < 10)
            break;
        snprintf(out, size, "%s %04x interval %u-%u attempt %u timeout %u", name, switch_bt_le16(params),
                 switch_bt_le16(params + 4), switch_bt_le16(params + 2), switch_bt_le16(params + 6),
                 switch_bt_le16(params + 8));
        return;
    case 0x080d:
        if (length < 4)
            break;
        snprintf(out, size, "%s %04x policy 0x%04x", name, switch_bt_le16(params), switch_bt_le16(params + 2));
        return;
    case 0x080f:
        if (length < 2)
            break;
        snprintf(out, size, "%s policy 0x%04x", name, switch_bt_le16(params));
        return;
    case 0x0811:
        if (length < 8)
            break;
        snprintf(out, size, "%s %04x latency %u remote timeout %u local timeout %u", name, switch_bt_le16(params),
                 switch_bt_le16(params + 2), switch_bt_le16(params + 4), switch_bt_le16(params + 6));
        return;
    case 0x0c1a:
        if (length < 1)
            break;
        snprintf(out, size, "%s 0x%02x (inquiry scan %u, page scan %u)", name, params[0], params[0] & 1,
                 (unsigned)params[0] >> 1 & 1);
        return;
    case 0x0c1c:
        if (length < 4)
            break;
        snprintf(out, size, "%s interval %u window %u", name, switch_bt_le16(params), switch_bt_le16(params + 2));
        return;
    case 0x0c37:
        if (length < 4)
            break;
        snprintf(out, size, "%s %04x timeout %u", name, switch_bt_le16(params), switch_bt_le16(params + 2));
        return;
    default: {  // Set Event Filter and anything new: the parameters in hex, at most 12 bytes
        char hex[25];
        size_t i = 0;
        for (; i < length && i < 12; ++i)
            snprintf(hex + 2 * i, 3, "%02x", params[i]);
        hex[2 * i] = '\0';
        snprintf(out, size, "%s [%s]", name, hex);
        return;
    }
    }
    snprintf(out, size, "%s (%04x) of %u bytes, too short", name, opcode, length);
}

#ifdef __cplusplus
}
#endif
