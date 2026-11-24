// SPDX-License-Identifier: BSD-2-Clause-Patent
// display.refresh_dictionary's view of IOMFBDisplayRefresh (debug kernels): the app's sandbox denies it the property
// (iokit-get-properties on AppleCLCD), so the display serves the six values the family published, read back once after
// the publish, as kern.switch_display_refresh_<field> (-1: the family published no such key). Included in the class
// body.

#if SWITCH_FRAME_TIMELINE
static constexpr unsigned kRefreshKeys = 6;
static inline int64_t refreshValues[kRefreshKeys] = {-1, -1, -1, -1, -1, -1};
static inline struct sysctl_oid refreshOids[kRefreshKeys] = {};

static int refreshSysctl(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    const int64_t value =
        __atomic_load_n(&refreshValues[static_cast<unsigned>(which) % kRefreshKeys], __ATOMIC_RELAXED);
    return SYSCTL_OUT(req, &value, sizeof(value));
}

void refreshValuesRegister()
{
    static const char *const keys[kRefreshKeys] = {"displayMinRefreshInterval", "displayMinRefreshIntervalMachTime",
                                                   "displayMaxRefreshInterval", "displayMaxRefreshIntervalMachTime",
                                                   "displayRefreshStep",        "displayRefreshStepMachTime"};
    static const char *const names[kRefreshKeys] = {"switch_display_refresh_min",  "switch_display_refresh_min_mach",
                                                    "switch_display_refresh_max",  "switch_display_refresh_max_mach",
                                                    "switch_display_refresh_step", "switch_display_refresh_step_mach"};
    const OSDictionary *refresh = OSDynamicCast(OSDictionary, getProperty("IOMFBDisplayRefresh"));
    for (unsigned i = 0; i < kRefreshKeys; ++i) {
        const OSNumber *number = refresh ? OSDynamicCast(OSNumber, refresh->getObject(keys[i])) : nullptr;
        __atomic_store_n(&refreshValues[i], number ? static_cast<int64_t>(number->unsigned64BitValue()) : -1,
                         __ATOMIC_RELAXED);
        if (refreshOids[i].oid_name)
            continue;
        switch_sysctl_oid(&refreshOids[i], &sysctl__kern_children, names[i],
                          static_cast<int>(CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED), refreshSysctl, "Q",
                          "IOMFBDisplayRefresh as the family published it");
        refreshOids[i].oid_arg2 = static_cast<int>(i);
        sysctl_register_oid(&refreshOids[i]);
    }
}
#else
void refreshValuesRegister()
{}
#endif
