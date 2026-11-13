// SPDX-License-Identifier: BSD-2-Clause-Patent
// The plug-in's SwitchEncoderLayoutProbe through VideoToolbox: one picture encoded, the probe read from the session,
// and every layout's IDR decoded on its own by Apple's decoder against that picture. Over the lossless fake engine each
// must come back exact.
#ifndef PROBE_CHECK_H
#define PROBE_CHECK_H
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stddef.h>

// One result line ("ok layout probe: 8 of 8 layouts, 8 decode back exactly"); `spec` names the encoder.
bool probe_check(CFDictionaryRef spec, char *line, size_t size);

#endif
