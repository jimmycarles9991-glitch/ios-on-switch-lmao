#ifndef SWITCH_FILES_H
#define SWITCH_FILES_H
#ifdef __cplusplus
extern "C" {
#endif
enum {
    SWITCH_STORAGE_FAT = 0,
    SWITCH_STORAGE_RAW = 1,
    SWITCH_STORAGE_INVALID = -1
};

/* The root boot reads the raw APFS container on the SD card; the FAT profile is gone. */
static inline int switch_files_storage_profile(void)
{
    return SWITCH_STORAGE_RAW;
}
#ifdef __cplusplus
}
#endif
#endif
