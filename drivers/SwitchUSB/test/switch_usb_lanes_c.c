// XusbLanes.h as C11 with -Wconversion -Wsign-conversion, as the kernel's C code and the USB host harness compile it.
#include "../XusbLanes.h"

// NOLINTNEXTLINE(misc-use-internal-linkage): external, so -Wunused-function leaves this compile check alone
bool switch_usb_lanes_c_compiles(void)
{
    return xusb_ss_lane_wanted(true, SWITCH_USB3_LANES_FREE, true) && xusb_mbox_ss_decide(17u, 2u).lfps &&
           xhci_imod_floor(0) == XHCI_IMOD_MIN_INTERVAL && xusb_ectl1(0, true) == 0x20027u &&
           xusb_ectl1(0, false) == 0x20000u && xusb_ss_lane(true) == 5u && xusb_ss_lane(false) == 6u &&
           xusb_ss_port_unmap(0u) == XUSB_SS_PORT_MAP_DISABLED &&
           xusb_transport_decide(XUSB_PORT_EMPTY, xusb_usb3_transport_wanted(XUSB_PORT_HOST, true, true),
                                 XUSB_PORT_HOST) == XUSB_TRANSPORT_ADD &&
           xusb_ss_xhci_port(1u, 4u) == 1u && xusb_host_asid_usable(0x8000007cu, 124u);
}
