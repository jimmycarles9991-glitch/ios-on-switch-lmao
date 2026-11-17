// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels: counts since boot that hw_test_app's playback and decode tests read around a clip.
//  - kern.switch_vic_maps, kern.switch_vic_remaps: SMMU mappings VIC made, and of them remakes of one it evicted for
//    room (a decoder pool or client surface pool larger than the map cache holds).
//  - kern.switch_native_objects, kern.switch_native_surfaces, kern.switch_native_publications: native publications that
//    needed a new surface object, those into a surface with no native picture before, and all of them.
#include "SwitchVICMapping.h"

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#include <sys/sysctl.h>
#include "SwitchNativeSurface.h"
#include "../../kits/SwitchSysctl/SwitchSysctl.h"

namespace {
enum : int {
    kMaps,
    kRemaps,
    kObjects,
    kSurfaces,
    kPublications,
    kTotals
};
struct sysctl_oid gOids[kTotals];

int readTotal(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    uint64_t made = 0, remade = 0, published = 0, objects = 0, surfaces = 0;
    switch_vic_mapping_totals(&made, &remade);
    switch_native_surface_publications(&published, &objects, &surfaces);
    const uint64_t totals[kTotals] = {made, remade, objects, surfaces, published};
    const int value = static_cast<int>(totals[static_cast<unsigned>(which) % kTotals]);
    return SYSCTL_OUT(req, &value, sizeof(value));
}
}  // namespace

void switch_vic_totals_register()
{
    static const char *const names[kTotals] = {"switch_vic_maps", "switch_vic_remaps", "switch_native_objects",
                                               "switch_native_surfaces", "switch_native_publications"};
    for (int i = 0; i < kTotals; ++i) {
        if (gOids[i].oid_name)
            continue;
        switch_sysctl_oid(&gOids[i], &sysctl__kern_children, names[i], CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
                          readTotal, "I",
                          "since boot: VIC mappings made, remade after eviction; native surface objects made, "
                          "surfaces first published into, publications");
        gOids[i].oid_arg2 = i;
        sysctl_register_oid(&gOids[i]);
    }
}
#else
void switch_vic_totals_register()
{}
#endif
