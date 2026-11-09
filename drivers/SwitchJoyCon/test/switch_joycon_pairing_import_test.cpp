// joyconpaird's import of Horizon's staged pairing dump (helper/joycon_pairing_import.h): once per dump, the rail's
// record wins over the dump's for the same Joy-Con, and the dump is marked imported only when every controller was
// stored or left to the rail, so a failure is retried at the next start.
#include <gtest/gtest.h>

#include <vector>

#include "../helper/joycon_pairing_import.h"

namespace {

struct Controller {
    bool wellFormed;
    bool railHas;
    int bluetoothd;  // the answer to a pairing request (0: stored)
};

struct Outcome {
    std::vector<enum switch_joycon_dump_entry> entries;
    unsigned paired;
    bool markImported;
    bool reportUnreadable;
};

Outcome Import(bool imported, bool readable, const std::vector<Controller> &controllers)
{
    Outcome outcome = {{}, 0, false, false};
    switch_joycon_dump_import import = switch_joycon_dump_begin(imported, readable);
    outcome.reportUnreadable = switch_joycon_dump_unreadable(&import);
    for (size_t i = 0; switch_joycon_dump_pending(&import) && i < controllers.size(); ++i) {
        const enum switch_joycon_dump_entry entry =
            switch_joycon_dump_classify(&import, controllers[i].wellFormed, controllers[i].railHas);
        outcome.entries.push_back(entry);
        if (entry == SWITCH_JOYCON_DUMP_PAIR) {
            ++outcome.paired;
            switch_joycon_dump_paired(&import, controllers[i].bluetoothd);
        }
    }
    outcome.markImported = switch_joycon_dump_mark_imported(&import);
    return outcome;
}

TEST(SwitchJoyConPairingImport, ADumpIsImportedOnce)
{
    const Outcome first = Import(false, true, {{true, false, 0}, {true, false, 0}});
    EXPECT_EQ(first.paired, 2u);
    EXPECT_TRUE(first.markImported);
    const Outcome again = Import(true, true, {{true, false, 0}, {true, false, 0}});
    EXPECT_EQ(again.paired, 0u);
    EXPECT_FALSE(again.markImported);
    EXPECT_FALSE(again.reportUnreadable);
}

TEST(SwitchJoyConPairingImport, TheRailsRecordWins)
{
    const Outcome outcome = Import(false, true, {{true, true, 0}, {true, false, 0}});
    ASSERT_EQ(outcome.entries.size(), 2u);
    EXPECT_EQ(outcome.entries[0], SWITCH_JOYCON_DUMP_RAIL_NEWER);
    EXPECT_EQ(outcome.entries[1], SWITCH_JOYCON_DUMP_PAIR);
    EXPECT_EQ(outcome.paired, 1u);
    EXPECT_TRUE(outcome.markImported) << "a Joy-Con left to the rail is not a failure";
}

TEST(SwitchJoyConPairingImport, AFailureIsRetried)
{
    EXPECT_FALSE(Import(false, true, {{true, false, 0}, {true, false, -1}}).markImported);
    const Outcome malformed = Import(false, true, {{false, false, 0}, {true, false, 0}});
    EXPECT_EQ(malformed.entries[0], SWITCH_JOYCON_DUMP_MALFORMED);
    EXPECT_EQ(malformed.paired, 1u) << "the other controllers are still imported";
    EXPECT_FALSE(malformed.markImported);
}

TEST(SwitchJoyConPairingImport, AnUnreadableDumpIsReportedAndNotMarked)
{
    const Outcome outcome = Import(false, false, {{true, false, 0}});
    EXPECT_TRUE(outcome.reportUnreadable);
    EXPECT_EQ(outcome.paired, 0u);
    EXPECT_FALSE(outcome.markImported);
    EXPECT_FALSE(Import(true, false, {}).reportUnreadable) << "a dump already imported is not read again";
}

TEST(SwitchJoyConPairingImport, AnEmptyDumpIsMarkedImported)
{
    EXPECT_TRUE(Import(false, true, {}).markImported);
}

}  // namespace
