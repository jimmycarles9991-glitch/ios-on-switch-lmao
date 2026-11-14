// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchThermal's DFLL mode (top of SwitchThermal.cpp): going on the DFLL, and what a failure does. Included by
// SwitchThermal.cpp; test/switch_thermal_dfll_test.cpp includes it with a stand-in SwitchThermal. Work loop only.
//
// Only -ENODEV from tegra_dfll_cpu_prepare (the chip's revision, speedo fuses or CVB row rule the DFLL out) ends the
// DFLL mode for the boot; the PLL ladder runs then, as with switch-no-dfll. Any other failure is one line and fails that
// raise: the CPU stays on PLLX at the inherited clock and the next raise (the next evaluation that wants the CPU up)
// runs this same path. A rate the DFLL refuses under the governor fails that request alone (dfllRefused).

// After SC7 (switch_thermal_resume): the DFLL's own registers, which enable needs. A failure is one line; the next raise
// restores them before it enables.
void SwitchThermal::resumeDfll()
{
    if (!dfllMode)
        return;
    char why[160] = "";
    const int err = tegra_dfll_cpu_resume(why, sizeof(why));
    dfllResume = err != 0;
    if (err)
        IOLog("SwitchPerf: DFLL registers not restored after sleep (%d: %s); the next raise restores them\n", err, why);
}

// The governor's rate `hz` refused by the DFLL, which keeps the rate it runs: that request fails, and the governor's
// next tick that wants the rate asks again. One line per refusal that differs from the last, until a rate lands.
void SwitchThermal::dfllRefused(unsigned long hz, int err)
{
    if (hz == dfllRefusedHz && err == dfllRefusedErr)
        return;
    dfllRefusedHz = hz;
    dfllRefusedErr = err;
    IOLog("SwitchPerf: the DFLL refused %lu MHz (%d); it keeps the rate it runs\n", hz / 1000000, err);
}

// DFLL mode raise: its registers back after SC7 if that failed, prepare once, the thermal indices for this
// temperature, then enable at 1785 MHz and the governor started.
bool SwitchThermal::raiseDfll(int soc)
{
    char why[256] = "";
    int err = 0;
    if (dfllResume) {
        err = tegra_dfll_cpu_resume(why, sizeof(why));
        if (err) {
            IOLog("SwitchPerf: DFLL not raised: its registers are not restored after sleep (%d: %s)\n", err, why);
            return false;
        }
        dfllResume = false;
    }
    err = dfllPrepared ? 0 : tegra_dfll_cpu_prepare(why, sizeof(why));
    if (err == -ENODEV) {
        dfllMode = governed = false;
        IOLog("SwitchPerf: the DFLL does not fit this chip (%s); PLL mode for this boot\n", why);
        return false;
    }
    if (!err) {
        dfllPrepared = true;
        dfllFloor = dfllCap = ~0u;
        updateDfllThermal(soc);
        err = tegra_dfll_cpu_enable(kDfllRateHz, why, sizeof(why));
    }
    if (err) {
        IOLog("SwitchPerf: DFLL not raised (%d: %s); the CPU stays at the inherited clock until the next raise\n", err,
              why);
        return false;
    }
    perfRaised = true;
    ++perfRaises;
    if (governed && !startGovernor(why, sizeof(why))) {
        restoreCpu("no governor", CpuDown::Again);
        IOLog("SwitchPerf: CPU governor not started (%s); the CPU stays at the inherited clock until the next raise\n",
              why);
        return false;
    }
    dfllRefusedHz = 0;
    char state[224];
    tegra_dfll_cpu_describe(kDfllRateHz, state, sizeof(state));
    const unsigned mhz = switchclk::readCpu(car).kHz / 1000;
    publish("CPUClockMHz", mhz);
    publish("CPURaises", perfRaises);
    IOLog("SwitchPerf: CPU on the DFLL at %lu MHz%s (raise %u; SoC %d.%d C): %s\n", kDfllRateHz / 1000000,
          governed ? ", on demand from here" : "", perfRaises, soc / 1000, soc % 1000 / 100, state);
    if (governed)
        IOLog("SwitchPerf: %s\n", why);  // the governor's table and tunables
    return true;
}
