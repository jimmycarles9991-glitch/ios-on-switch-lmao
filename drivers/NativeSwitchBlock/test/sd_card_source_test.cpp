// Guards on NativeSwitchBlock.cpp's use of the SD card's registers (sd_card_test.cpp tests the decoders and encodings;
// the driver is not built for the host): identification reads the card's registers once at boot and writes nothing,
// and auto CMD23 is used only where the card lists it (r14-sdcq).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::Body;
using switch_ios::test::Index;
using switch_ios::test::Search;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
const std::string &driver()
{
    static const std::string source = SourceText("../NativeSwitchBlock.cpp");
    return source;
}

std::string body(const char *signature)
{
    return Body(driver(), signature);
}
}  // namespace

// identify() reads the card's registers once at boot and writes nothing: no CMD49, no data written.
TEST(SdCardSource, IdentificationIsReadOnly)
{
    const std::string identify = body("void NativeSwitchBlock::identify()");
    for (const char *command : {"appCommand(51,", "appCommand(13,", "extRead(0, 0, kSector, regs)"})
        EXPECT_THAT(identify, HasSubstr(command));
    EXPECT_THAT(identify, Not(HasSubstr("cardCommand(49")));
    EXPECT_THAT(identify, Not(HasSubstr("w16(COMMAND")));
    const std::string ext = body("bool NativeSwitchBlock::extRead(");
    EXPECT_THAT(ext, HasSubstr("cardCommand(48, switchSdExtArgument(fno, address, length)"));
    EXPECT_THAT(ext, HasSubstr("if (!switchSdExtRange(address, length))\n        return false;"));
    // Once at boot, right after the bus is up (so the identification lines come first), never on resume.
    const std::string setup = body("bool NativeSwitchBlock::setup()");
    EXPECT_LT(Index(setup, "raiseBusSpeed()"), Index(setup, "identify();"));
    EXPECT_LT(Index(setup, "identify();"), Index(setup, "setupAdma()"));
    EXPECT_THAT(body("void NativeSwitchBlock::resume()"), Not(HasSubstr("identify();")));
    const std::string reinit = body("bool NativeSwitchBlock::reinitialize(");
    EXPECT_THAT(reinit, HasSubstr("if (!resume && !cardCommand(9, rca << 16, RSP_R2, nullptr))"));
    EXPECT_LT(Index(reinit, "cardCommand(9, rca << 16"), Index(reinit, "cardCommand(7, rca << 16"));
}

// Auto CMD23 only for multi-block ADMA2 commands when the SCR lists CMD23; retries and every SDMA command (bounce path,
// proofs, setup reads) stop by CMD12 as before.
TEST(SdCardSource, Cmd23FramingIsGated)
{
    const std::string &source = driver();
    const std::string issue = body("int NativeSwitchBlock::issue(");
    EXPECT_THAT(issue, HasSubstr("switchSdBlockFrame(sectors, write, cmd23)"));
    // Argument 2 before the command that uses it, and the partition guard still first.
    EXPECT_LT(Index(issue, "outside its partition"), Index(issue, "w32(ARGUMENT2, frame.argument2)"));
    EXPECT_LT(Index(issue, "w32(ARGUMENT2, frame.argument2)"), Index(issue, "w16(COMMAND, frame.command)"));
    const std::string complete = body("int NativeSwitchBlock::complete(");
    EXPECT_THAT(complete, HasSubstr("if (r16(AUTO_CMD_ERROR) & 0x9f)\n            return 8;"));
    EXPECT_THAT(complete, HasSubstr("if (!cmd23 && (r32(STOP_RESPONSE) & R1_STOP_ERRORS))\n            return 9;"));
    // The data path: direct (ADMA2) multi-block commands only.
    EXPECT_THAT(body("void NativeSwitchBlock::prepareAndPublishSlot("),
                HasSubstr("b.cmd23 = b.direct && sectors > 1 && autoCmd23;"));
    // Retries go back to CMD12 (command()'s default), as do transfer()/run() (the bounce path and the proofs).
    EXPECT_THAT(body("int NativeSwitchBlock::retry("),
                HasSubstr("step = command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status, false, "
                          "window, descPA);"));
    EXPECT_THAT(body("int NativeSwitchBlock::run("),
                HasSubstr("command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status, false, window);"));
    EXPECT_TRUE(Search(source, R"(int command\(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, )"
                               R"(uint32_t \*status, bool cmd23 = false,\s*const SwitchSdWindow \*window = nullptr,\s*)"
                               R"(uint64_t descPA = 0\);)"));
    // setup(): from the registers (switchSdPath: ADMA2, and the SCR listing CMD23).
    EXPECT_THAT(body("bool NativeSwitchBlock::setup()"),
                HasSubstr("autoCmd23 = switchSdPath(r32(CAPABILITIES), r16(HOST_CONTROL2), haveScr ? &scr : nullptr)"));
}

// Volatile write cache: enabled only with Power Off Notification support, FUA withdrawn when on, doSynchronize flushes,
// suspend/shutdown flush before PON, and resume re-enables cache after power cycle.
TEST(SdCardSource, WriteCacheContract)
{
    const std::string configure = body("bool NativeSwitchBlockDevice::configure(");
    EXPECT_THAT(configure, HasSubstr("if (!provider->writeCacheEnabled())"));
    EXPECT_THAT(body("IOReturn NativeSwitchBlockDevice::doSynchronize("), HasSubstr("owner->synchronize(window)"));
    EXPECT_THAT(body("IOReturn NativeSwitchBlockDevice::doSynchronizeCache()"),
                HasSubstr("owner->synchronize(window)"));
    EXPECT_THAT(body("IOReturn NativeSwitchBlock::synchronize("), HasSubstr("syncLocked(window)"));
    EXPECT_THAT(body("IOReturn NativeSwitchBlockDevice::getWriteCacheState("), HasSubstr("owner->writeCacheEnabled()"));
    const std::string setup = body("bool NativeSwitchBlock::setup()");
    EXPECT_LT(Index(setup, "identify();"), Index(setup, "configureCache();"));
    const std::string cfgCache = body("void NativeSwitchBlock::configureCache()");
    EXPECT_THAT(cfgCache, HasSubstr("perf.cache && havePower && power.offNotification"));
    EXPECT_THAT(cfgCache, HasSubstr("disableFoundCache();"));
    const std::string suspend = body("void NativeSwitchBlock::suspend()");
    EXPECT_LT(Index(suspend, "flushCache()"), Index(suspend, "powerOffNotify();"));
    EXPECT_LT(Index(suspend, "powerOffNotify();"), Index(suspend, "powerOffCard();"));
    const std::string shutdown = body("void NativeSwitchBlock::shutdown()");
    EXPECT_LT(Index(shutdown, "flushCache()"), Index(shutdown, "powerOffNotify();"));
    const std::string resume = body("void NativeSwitchBlock::resume()");
    EXPECT_LT(Index(resume, "dmaResume();"), Index(resume, "enableCache(true)"));
}

// UHS DDR200 uses PLLC4_OUT0 / 2.5 with SDHCI /2 in UHS_DDR50 mode, manual 128-tap tuning (>= 8 taps), and falls back
// cleanly to SDR104 (with power-cycle at boot, or CMD6 step-down at runtime).
TEST(SdCardSource, Ddr200HasFallbackToSdr104)
{
    const std::string reinit = body("bool NativeSwitchBlock::reinitialize(");
    EXPECT_THAT(reinit,
                HasSubstr("setTiming(UHS_DDR50, false, SOURCE_PLLC4_OUT0, kDdr200CarDivider, kDdr200SdhciDivider)"));
    EXPECT_THAT(reinit, HasSubstr("tuneDdr200(&ddr200Window)"));
    EXPECT_THAT(reinit, HasSubstr("powerOffCard();"));
    EXPECT_THAT(reinit, HasSubstr("powerOnController();"));
    EXPECT_THAT(body("void NativeSwitchBlock::retune("), HasSubstr("switchFunction(true, 1, 0)"));
    EXPECT_THAT(body("bool NativeSwitchBlock::raiseBusSpeed()"), HasSubstr("switch-no-sd-ddr200"));
}
