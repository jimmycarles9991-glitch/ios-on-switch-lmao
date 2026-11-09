// SPDX-License-Identifier: BSD-2-Clause-Patent
// joyconpaird's rules for importing Horizon's staged pairing dump (joyconpaird.c importStaged; the host test
// test/switch_joycon_pairing_import_test.cpp drives this header). The agent supplies what it read: whether the dump
// equals the copy kept at the last complete import, whether it holds a Controllers array, and per controller whether
// the entry is well formed and whether the rail has a record for its address. These rules say which controllers go to
// bluetoothd and whether the dump is marked imported.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct switch_joycon_dump_import {
    bool imported;  // the staged dump equals the copy kept at its last complete import
    bool readable;  // it is a dictionary with a Controllers array
    unsigned failures;
};

enum switch_joycon_dump_entry {
    SWITCH_JOYCON_DUMP_PAIR,        // hand the key to bluetoothd
    SWITCH_JOYCON_DUMP_RAIL_NEWER,  // the rail's record, just read from the Joy-Con, wins over the dump's
    SWITCH_JOYCON_DUMP_MALFORMED,
};

static inline struct switch_joycon_dump_import switch_joycon_dump_begin(bool imported, bool readable)
{
    struct switch_joycon_dump_import import = {imported, readable, 0};
    return import;
}

// Whether the dump's controllers are looked at at all: once per dump, and only a readable one.
static inline bool switch_joycon_dump_pending(const struct switch_joycon_dump_import *import)
{
    return !import->imported && import->readable;
}

static inline enum switch_joycon_dump_entry switch_joycon_dump_classify(struct switch_joycon_dump_import *import,
                                                                        bool well_formed, bool rail_has)
{
    if (!well_formed) {
        ++import->failures;
        return SWITCH_JOYCON_DUMP_MALFORMED;
    }
    return rail_has ? SWITCH_JOYCON_DUMP_RAIL_NEWER : SWITCH_JOYCON_DUMP_PAIR;
}

// bluetoothd's answer to a SWITCH_JOYCON_DUMP_PAIR entry (0: stored).
static inline void switch_joycon_dump_paired(struct switch_joycon_dump_import *import, int result)
{
    if (result)
        ++import->failures;
}

// Whether to keep a copy of the dump as imported: only after every controller of a readable dump was stored or left to
// the rail, so a failed import is tried again at the next start and a later boot never puts back a key the rail has
// replaced since.
static inline bool switch_joycon_dump_mark_imported(const struct switch_joycon_dump_import *import)
{
    return switch_joycon_dump_pending(import) && !import->failures;
}

// Whether the staged file is a problem worth an error line: present, not yet imported, and not a pairing dump.
static inline bool switch_joycon_dump_unreadable(const struct switch_joycon_dump_import *import)
{
    return !import->imported && !import->readable;
}

#ifdef __cplusplus
}
#endif
