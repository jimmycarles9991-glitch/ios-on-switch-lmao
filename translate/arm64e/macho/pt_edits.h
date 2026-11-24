#ifndef A64X_PT_EDITS_H
#define A64X_PT_EDITS_H

#include "pretranslate.h"

int a64x_pt_apply_edits(const struct a64x_macho *m, const uint8_t *data, uint64_t tlo, uint64_t thi, uint8_t *out,
                        const struct a64x_pt_edit *edits, size_t nedits, char *err, size_t errlen);

int a64x_pt_check_edits(const struct a64x_macho *m, const uint8_t *data, const uint8_t *freed, uint64_t tlo,
                        uint64_t thi, const uint8_t *out, const struct a64x_pt_edit *edits, size_t nedits, char *err,
                        size_t errlen);

bool a64x_pt_edited_word(const struct a64x_pt_edit *edits, size_t nedits, uint64_t off, uint32_t *expected);

#endif
