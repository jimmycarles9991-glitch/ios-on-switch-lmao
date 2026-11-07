// switch_usb_host_harness.c's exported wrappers of drivers/SwitchUSB/SwitchPDPolicy.h and XusbPorts.h (and its fake
// xHCI capability space), for switch_usb_host_test.cpp.
#ifndef SWITCH_SWITCH_USB_HOST_HARNESS_H
#define SWITCH_SWITCH_USB_HOST_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- SwitchPDPolicy.h ---
int pd_decide(int phase, uint16_t alert, uint16_t status1, uint16_t status2, int allow, int *next, int *cancel,
              char *why, size_t why_size);
int pd_window_end(int phase, int saw_detach, uint16_t status1, int *next, char *why, size_t why_size);
int pd_switches_on(int phase, int allow);
int pd_rails_on(int phase);
int pd_vbus_ours(uint16_t status1);
const char *pd_phase_name(int phase);
int pd_usb_host(int sourcing, uint16_t status1);

// --- XusbPorts.h ---
void fake_reset(void);
void fake_write(uint32_t offset, uint32_t value);
// first << 16 | count, and the number of reads the walk took in *reads.
uint32_t find_ports(uint32_t window, unsigned major, unsigned *reads);
uint32_t portsc_offset(uint32_t cap_length, unsigned port);
uint32_t portsc_power(uint32_t portsc, int on);
uint32_t portsc_keep(void);
uint32_t vbus_id_drop_vbus(uint32_t v);
uint32_t vbus_id_host(uint32_t v);
uint32_t vbus_id_device(uint32_t v);
uint32_t chrg_ctl0_host(uint32_t v);
uint32_t chrg_ctl1_host(uint32_t v);
uint32_t oc_map_host(uint32_t v);
const char *id_name(uint32_t v);
// verdict of xusb_irq_judge for an armed and a current sample
int irq_judge(uint32_t armed_count, uint32_t armed_erdp, int armed_pending, int armed_deliverable, uint32_t now_count,
              uint32_t now_erdp, int now_pending, int now_deliverable, int first, unsigned stage);
int xhci_pending(uint32_t iman, uint32_t usbsts);
int xhci_deliverable(uint32_t iman, uint32_t usbcmd);
const char *portsc_link_name(uint32_t portsc);
const char *portsc_speed_name(uint32_t portsc);

#ifdef __cplusplus
}
#endif

#endif  // SWITCH_SWITCH_USB_HOST_HARNESS_H
