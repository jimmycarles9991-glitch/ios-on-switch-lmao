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

int pd_window_end(int phase, int saw_detach, uint16_t status1, int *next, char *why, size_t why_size)
{
    const struct switch_pd_step step = switch_pd_window_end((enum switch_pd_phase)phase, saw_detach != 0, status1);
    *next = (int)step.next;
    copy_why(step.why, why, why_size);
    return (int)step.act;
}

int pd_switches_on(int phase, int allow)
{
    return switch_pd_switches_on((enum switch_pd_phase)phase, allow != 0);
}

int pd_rails_on(int phase)
{
    return switch_pd_rails_on((enum switch_pd_phase)phase);
}

int pd_vbus_ours(uint16_t status1)
{
    return switch_pd_vbus_ours(status1);
}

const char *pd_phase_name(int phase)
{
    return switch_pd_phase_name((enum switch_pd_phase)phase);
}

int pd_usb_host(int sourcing, uint16_t status1)
{
    return switch_pd_usb_host(sourcing != 0, status1);
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

// verdict of xusb_irq_judge for an armed and a current sample
int irq_judge(uint32_t armed_count, uint32_t armed_erdp, int armed_pending, int armed_deliverable, uint32_t now_count,
              uint32_t now_erdp, int now_pending, int now_deliverable, int first, unsigned stage)
{
    const struct xusb_irq_sample armed = {armed_count, armed_erdp, armed_pending != 0, armed_deliverable != 0};
    const struct xusb_irq_sample now = {now_count, now_erdp, now_pending != 0, now_deliverable != 0};
    return (int)xusb_irq_judge(armed, now, first != 0, stage);
}

int xhci_pending(uint32_t iman, uint32_t usbsts)
{
    return xusb_xhci_pending(iman, usbsts);
}

int xhci_deliverable(uint32_t iman, uint32_t usbcmd)
{
    return xusb_xhci_deliverable(iman, usbcmd);
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
    return xusb_hold_keeps(&hold, value, current);
}

uint32_t hold_erstba0(void)
{
    return XHCI_ERSTBA0;
}
