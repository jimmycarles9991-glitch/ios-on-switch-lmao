// SPDX-License-Identifier: BSD-2-Clause-Patent
#if DEVELOPMENT || DEBUG
#include <IOKit/IOLib.h>
#include "SwitchWLANA3.h"
#include "firmware/rom_capture.h"
#include "firmware/a3/image.h"
#include "firmware/a3/payload.h"
#include "firmware/a3/payload_candidate.h"

uint32_t switch_wlan_a3_state_address(bool candidate)
{
    return candidate ? switch_wlan_a3_candidate_state : switch_wlan_a3_state;
}

uint8_t *switch_wlan_a3_build(const uint8_t *image, size_t length, size_t *outputLength, bool candidate)
{
    *outputLength = 0;
    // `image` is the firmware the kext embeds (firmware.S), which rom_capture_test pins to these patches at build time
    if (!image || length != SWITCH_WLAN_ROM_IMAGE_BYTES)
        return nullptr;
    const size_t size = (candidate ? switch_wlan_a3_candidate_end : switch_wlan_a3_end) - SWITCH_A3_IMAGE_BASE;
    auto *output = static_cast<uint8_t *>(IOMalloc(size));
    if (!output)
        return nullptr;
    const switch_a3_candidate_entries entries = {
        switch_wlan_a3_candidate_heap_init,       switch_wlan_a3_candidate_attach,
        switch_wlan_a3_candidate_rx_entry,        switch_wlan_a3_candidate_tx_entry,
        switch_wlan_a3_candidate_tx_radio_status, switch_wlan_a3_candidate_pm_status_entry,
        switch_wlan_a3_candidate_channel_gate,    switch_wlan_a3_candidate_pm_prepare_entry,
        switch_wlan_a3_candidate_pm_submit_entry, switch_wlan_a3_candidate_pm_set_entry,
    };
    const bool patched =
        candidate
            ? switch_a3_patch_candidate_image(image, length, output, size, switch_wlan_a3_candidate_payload,
                                              sizeof switch_wlan_a3_candidate_payload, &entries)
            : switch_a3_patch_image(image, length, output, size, switch_wlan_a3_payload, sizeof switch_wlan_a3_payload,
                                    switch_wlan_a3_heap_init, switch_wlan_a3_attach, switch_wlan_a3_rx_entry);
    if (!patched) {
        IOFree(output, size);
        IOLog("SwitchWLAN: A3 backend refused: patch site changed\n");
        return nullptr;
    }
    *outputLength = size;
    IOLog("SwitchWLAN: A3 backend %s: %u bytes reserved at %#x\n", candidate ? "candidate" : "bootstrap",
          static_cast<unsigned>(candidate ? sizeof switch_wlan_a3_candidate_payload : sizeof switch_wlan_a3_payload),
          switch_wlan_a3_begin);
    return output;
}
#endif
