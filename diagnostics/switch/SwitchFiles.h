#ifndef SWITCH_FILES_H
#define SWITCH_FILES_H
#include <stdint.h>
#include "storage/sdmmc_read.h"
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
struct switch_raw_apfs;
int switch_files_raw_open(struct switch_raw_apfs *, void *private_scratch, uint32_t);
#define SWITCH_FILES_MAGIC UINT64_C(0x5357434846494c45)
/* magic,version,stage,error,FATerror,SDerror,capacity,partitionstart,
 * partitionsize,clusterbytes,sectorsread,lastLBA,SDstatus,SDstage,readymask,
 * blocked,fixturebytes,fixtureverified,volumesopened,rangesverified,
 * rangeindex,volumeindex,rangeoffset,rangelength,verifiedbytes,elapsedticks,
 * frequency,sectorbudget,secondsbudget,SHAkat,totalranges,totalvolumes,
 * GPTerror,partitionindex,BPBsectors,GPTentrycount,GPTentrysize,reserved[3]. */
extern volatile uint64_t switch_files_record[40];
extern uint8_t switch_files_digest[32];
extern uint8_t switch_files_expected_digest[32];
void switch_files_probe(struct switch_sd_host *);
void switch_diag_files_update(void);
#ifdef __cplusplus
}
#endif
#endif
