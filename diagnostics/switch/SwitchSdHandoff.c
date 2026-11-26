// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../../drivers/NativeSwitchBlock/SwitchSdVolumeInternal.h"
#include <mach/vm_prot.h>
#include <machine/machine_routines.h>

static const volatile uint8_t *switch_sd_handoff_page(void)
{
    // The one controller serializes probe and resume. ml_io_map_with_prot makes a permanent mapping.
    static vm_offset_t page;
    if (!page)
        page = ml_io_map_with_prot(SWITCH_SD_REFERENCE_PA, SWITCH_SD_REFERENCE_BYTES, VM_PROT_READ);
    return (const volatile uint8_t *)page;
}

bool switch_sd_boot_card_cid(uint8_t cid[16])
{
    return switch_sd_handoff_cid(switch_sd_handoff_page(), cid);
}

int switch_sd_files_volume(switch_sd_read_sector_fn read, void *context, uint64_t root_first, uint64_t root_sectors,
                           struct switch_sd_files_slice *result)
{
    static struct switch_gpt_workspace workspace;
    return switch_sd_files_select(read, context, switch_sd_handoff_page(), root_first, root_sectors, &workspace,
                                  result);
}
