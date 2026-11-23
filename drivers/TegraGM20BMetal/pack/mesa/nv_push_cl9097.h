/*
 * The FERMI_A (9097) method numbers mme_fermi_sim.c uses, as Mesa's generated nv_push_cl9097.h defines them from NVIDIA's
 * open class headers (MIT). r8-rawca: no method-name dumps here (P_PARSE/P_DUMP print "?").
 */
#ifndef SWITCH_METAL_NV_PUSH_CL9097_H
#define SWITCH_METAL_NV_PUSH_CL9097_H
#include <stdio.h>
#define NV9097_SET_REPORT_SEMAPHORE_A 0x1b00
#define NV9097_SET_REPORT_SEMAPHORE_B 0x1b04
#define NV9097_SET_REPORT_SEMAPHORE_C 0x1b08
#define NV9097_SET_REPORT_SEMAPHORE_D 0x1b0c
#define NV9097_SET_MME_SHADOW_SCRATCH(i) (0x3400 + (i) * 4)
#define NV9097_CALL_MME_MACRO(j) (0x3800 + (j) * 8)
static inline const char *P_PARSE_NV9097_MTHD(uint16_t mthd)
{
    (void)mthd;
    return "?";
}
static inline void P_DUMP_NV9097_MTHD_DATA(FILE *fp, uint16_t mthd, uint32_t data, const char *prefix)
{
    fprintf(fp, "%s%04x = %08x\n", prefix, mthd, data);
}
#endif
