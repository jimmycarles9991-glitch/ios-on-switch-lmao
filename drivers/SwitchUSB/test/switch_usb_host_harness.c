// Host harness for drivers/SwitchUSB/SwitchPDPolicy.h and XusbPorts.h: the headers' static inline functions as exported
// C ones (C11, -Wconversion -Wsign-conversion), plus a fake xHCI capability space for the port lookup, driven by
// switch_usb_host_test.cpp.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../SwitchPDPolicy.h"
#include "../XusbPorts.h"
#include "switch_usb_host_harness.h"

// --- SwitchPDPolicy.h ---
static void copy_why(const char *why, char *out, size_t size)
{
    if (!out || !size)
        return;
    out[0] = 0;
    if (why) {
        strncpy(out, why, size - 1);
        out[size - 1] = 0;
    }
}

int pd_decide(int phase, uint16_t alert, uint16_t status1, uint16_t status2, int allow, int *next, int *cancel,
              char *why, size_t why_size)
{
    const struct switch_pd_step step =
        switch_pd_decide((enum switch_pd_phase)phase, alert, status1, status2, allow != 0);
    *next = (int)step.next;
    *cancel = step.cancel_window ? 1 : 0;
    copy_why(step.why, why, why_size);
    return (int)step.act;
}

int pd_window_end(int phase, int saw_detach, uint16_t status1, uint16_t status2, int *next, char *why, size_t why_size)
{
    const struct switch_pd_step step =
        switch_pd_window_end((enum switch_pd_phase)phase, saw_detach != 0, status1, status2);
    *next = (int)step.next;
    copy_why(step.why, why, why_size);
    return (int)step.act;
}

int pd_switches_on(int phase, int allow)
{
    return switch_pd_switches_on((enum switch_pd_phase)phase, allow != 0) ? 1 : 0;
}

int pd_vbus_rail_on(int phase)
{
    return switch_pd_vbus_rail_on((enum switch_pd_phase)phase) ? 1 : 0;
}

int pd_vconn_on(int phase, uint16_t status1, uint16_t status2)
{
    return switch_pd_vconn_on((enum switch_pd_phase)phase, status1, status2) ? 1 : 0;
}

int pd_boost_on(int phase)
{
    return switch_pd_boost_on((enum switch_pd_phase)phase) ? 1 : 0;
}

int pd_vbus_ours(uint16_t status1)
{
    return switch_pd_vbus_ours(status1) ? 1 : 0;
}

const char *pd_phase_name(int phase)
{
    return switch_pd_phase_name((enum switch_pd_phase)phase);
}

int pd_usb_host(int sourcing, uint16_t status1)
{
    return switch_pd_usb_host(sourcing != 0, status1) ? 1 : 0;
}

int pd_usb_device_vbus(int sourcing, uint16_t status1)
{
    return switch_pd_usb_device_vbus(sourcing != 0, status1) ? 1 : 0;
}

int pd_reset_at_start(uint16_t status1, uint16_t dp_status)
{
    return switch_pd_reset_at_start(status1, dp_status) ? 1 : 0;
}

int pd_reset_on_fault(uint16_t alert, uint16_t status1)
{
    return switch_pd_reset_on_fault(alert, status1) ? 1 : 0;
}

int pd_hard_reset(uint16_t alert, uint16_t status1)
{
    return switch_pd_hard_reset(alert, status1) ? 1 : 0;
}

// --- XusbPorts.h ---
static uint32_t fake_space[0x2000 / 4];

void fake_reset(void)
{
    memset(fake_space, 0, sizeof(fake_space));
}

void fake_write(uint32_t offset, uint32_t value)
{
    if (offset / 4 < sizeof(fake_space) / 4)
        fake_space[offset / 4] = value;
}

static uint32_t fake_read(void *context, uint32_t offset)
{
    unsigned *reads = context;
    ++*reads;
    return offset / 4 < sizeof(fake_space) / 4 ? fake_space[offset / 4] : 0xffffffffu;
}

// first << 16 | count, and the number of reads the walk took in *reads.
uint32_t find_ports(uint32_t window, unsigned major, unsigned *reads)
{
    *reads = 0;
    const struct xhci_port_range range = xhci_find_ports(&fake_read, reads, window, major);
    return range.first << 16 | range.count;
}

int usb2_hlc(uint32_t window)
{
    unsigned reads = 0;
    return xhci_usb2_hlc(&fake_read, &reads, window) ? 1 : 0;
}

uint32_t portsc_offset(uint32_t cap_length, unsigned port)
{
    return xhci_portsc_offset(cap_length, port);
}

uint32_t portsc_power(uint32_t portsc, int on)
{
    return xhci_portsc_power(portsc, on != 0);
}

uint32_t portsc_keep(void)
{
    return XHCI_PORTSC_KEEP;
}

uint32_t vbus_id_drop_vbus(uint32_t v)
{
    return xusb_vbus_id_drop_vbus(v);
}

uint32_t vbus_id_host(uint32_t v)
{
    return xusb_vbus_id_host(v);
}

uint32_t vbus_id_device(uint32_t v)
{
    return xusb_vbus_id_device(v);
}

uint32_t chrg_ctl0_host(uint32_t v)
{
    return xusb_chrg_ctl0_host(v);
}

uint32_t chrg_ctl1_host(uint32_t v)
{
    return xusb_chrg_ctl1_host(v);
}

uint32_t oc_map_host(uint32_t v)
{
    return xusb_oc_map_host(v);
}

const char *id_name(uint32_t v)
{
    return xusb_id_name(v);
}

const char *portsc_link_name(uint32_t portsc)
{
    return xhci_portsc_link_name(portsc);
}

const char *portsc_speed_name(uint32_t portsc)
{
    return xhci_portsc_speed_name(portsc);
}

// --- XusbPorts.h xusb_hold ---
static struct xusb_hold hold;

void hold_reset(void)
{
    memset(&hold, 0, sizeof(hold));
}

uint32_t hold_usbcmd(uint32_t value)
{
    return xusb_hold_usbcmd(&hold, value);
}

uint32_t hold_usbsts(uint32_t value)
{
    return xusb_hold_usbsts(&hold, value);
}

void hold_usbsts_write(uint32_t value)
{
    xusb_hold_usbsts_write(&hold, value);
}

void hold_lost(void)
{
    xusb_hold_lost(&hold);
}

int hold_keeps(uint64_t value, uint64_t current)
{
    return xusb_hold_retained(&hold) && value == current;
}

uint32_t hold_erstba0(void)
{
    return XHCI_ERSTBA0;
}
