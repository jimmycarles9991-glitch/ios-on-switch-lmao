// XusbLanes.h as C11 with -Wconversion -Wsign-conversion, as the kernel's C code and the USB host harness compile it.
#include "../XusbLanes.h"

bool switch_usb_lanes_c_compiles(void)
{
    return xusb_ss_lane_wanted(true, SWITCH_USB3_LANES_FREE, true) && xusb_mbox_ss_decide(17u, 2u).lfps &&
           xhci_imod_floor(0) == XHCI_IMOD_MIN_INTERVAL && xusb_ectl1(0) == 0x20027u &&
           xusb_ss_port_follow(true, true, false, true).usb2_cycle && xusb_ss_xhci_port(1u, 4u) == 1u &&
           xusb_host_asid_usable(0x8000007cu, 124u);
}
