// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for tegra_nvenc_latch (TegraNVENCCheckCommon.h), what TegraNVENC writes in the colocated and reference
// slot 1-15 methods a job leaves out: 0 in IN_COLOC_DATA and nothing else until a job names one, then the scratch
// buffer in each one named since, and nothing in those the job names itself. Prints "PASS nvenclatch" when all hold.
#include <stdio.h>
#include "../../TegraNVENC/TegraNVENCCheckCommon.h"

static int gFailures;

#define CHECK(cond, ...)                  \
    do {                                  \
        if (!(cond)) {                    \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");                 \
            gFailures++;                  \
        }                                 \
    } while (0)

static TegraNVENCSubmit job(const uint16_t *methods, uint32_t count)
{
    TegraNVENCSubmit s = {.codec = TEGRA_NVENC_CODEC_H264, .entry_count = count};
    for (uint32_t i = 0; i < count; i++)
        s.entries[i] = (TegraNVENCEntry){methods[i], TEGRA_NVENC_ENTRY_RELOC, 1, 0};
    return s;
}

// The method latched slot k is, and what each job of the sequence gets there (Z: 0, S: scratch, -: nothing).
static void expect(const char *name, const TegraNVENCSubmit *s, uint32_t *latched, const char *actions)
{
    uint8_t action[TEGRA_NVENC_LATCHED];
    tegra_nvenc_latch(s, latched, action);
    for (uint32_t k = 0; k < TEGRA_NVENC_LATCHED; k++) {
        const char got = "-ZS"[action[k]];
        CHECK(got == actions[k], "%s: method %#x gets %c, expected %c", name, tegra_nvenc_latched_method(k), got,
              actions[k]);
    }
}

int main(void)
{
    CHECK(tegra_nvenc_latched_method(0) == NVC5B7_SET_IN_COLOC_DATA &&
              tegra_nvenc_latched_method(1) == NVC5B7_SET_OUT_COLOC_DATA &&
              tegra_nvenc_latched_method(2) == NVC5B7_SET_IN_REF_PIC1_LUMA &&
              tegra_nvenc_latched_method(16) == NVC5B7_SET_IN_REF_PIC15_LUMA,
          "the latched methods are the colocated pair and reference slots 1-15");
    uint32_t latched = 0;
    const uint16_t p[] = {NVC5B7_SET_IN_CUR_PIC, NVC5B7_SET_IN_REF_PIC0_LUMA};
    const uint16_t b[] = {NVC5B7_SET_IN_REF_PIC0_LUMA, NVC5B7_SET_IN_REF_PIC1_LUMA, NVC5B7_SET_IN_COLOC_DATA,
                          NVC5B7_SET_OUT_COLOC_DATA};
    const uint16_t anchor[] = {NVC5B7_SET_IN_REF_PIC1_LUMA, NVC5B7_SET_OUT_COLOC_DATA};
    const TegraNVENCSubmit pJob = job(p, 2), bJob = job(b, 4), anchorJob = job(anchor, 2);
    expect("P before any B", &pJob, &latched, "Z----------------");
    expect("P again", &pJob, &latched, "Z----------------");
    expect("B", &bJob, &latched, "-----------------");
    expect("an anchor naming slot 1", &anchorJob, &latched, "S----------------");
    expect("P after B", &pJob, &latched, "SSS--------------");
    latched = 0;  // the falcon booted again
    expect("P after a boot", &pJob, &latched, "Z----------------");
    if (gFailures) {
        printf("FAIL nvenclatch: %d failures\n", gFailures);
        return 1;
    }
    printf("PASS nvenclatch\n");
    return 0;
}
