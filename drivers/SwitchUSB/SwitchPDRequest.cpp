// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPD's USB-PD request within a sink session (SwitchPD.h): bm92txx.c bm92t_event_handler's
// INIT_STATE/NEW_PDO/PS_RDY_SENT steps and bm92t_power_work, bm92t_check_pdo/bm92t_send_rdo through
// SwitchBattery/SwitchChargePolicy.h; its msleep()s are one-shot timers here.
#include "SwitchPDService.h"

using namespace switchpd;

namespace {
uint32_t le32(const uint8_t *b)
{
    return static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 | static_cast<uint32_t>(b[2]) << 16 |
           static_cast<uint32_t>(b[3]) << 24;
}

// "15", "1.2", "2.25": milli-units as units without trailing zeros.
void formatMilli(char *out, size_t size, unsigned milli)
{
    const unsigned whole = milli / 1000;
    const unsigned frac = milli % 1000;
    if (!frac) {
        snprintf(out, size, "%u", whole);
    } else if (frac % 100 == 0) {
        snprintf(out, size, "%u.%u", whole, frac / 100);
    } else if (frac % 10 == 0) {
        snprintf(out, size, "%u.%02u", whole, frac / 10);
    } else {
        snprintf(out, size, "%u.%03u", whole, frac);
    }
}

// "5 V 1.5 A, *15 V 2.6 A" (* = position `chosen`); other PDO types by name.
void formatPdos(char *out, size_t size, const uint32_t *pdos, unsigned count, unsigned chosen)
{
    size_t used = 0;
    out[0] = 0;
    for (unsigned i = 0; i < count && used + 1 < size; ++i) {
        char volts[12];
        char amps[12];
        formatMilli(volts, sizeof(volts), charge::pdoMillivolts(pdos[i]));
        formatMilli(amps, sizeof(amps), charge::pdoMilliamps(pdos[i]));
        static const char *const kTypes[] = {"", "battery ", "variable ", "augmented "};
        const int n = charge::pdoType(pdos[i]) == 3
                          ? snprintf(out + used, size - used, "%saugmented", i ? ", " : "")
                          : snprintf(out + used, size - used, "%s%s%s%s V %s A", i ? ", " : "",
                                     i + 1 == chosen ? "*" : "", kTypes[charge::pdoType(pdos[i])], volts, amps);
        if (n < 0)
            break;
        used += static_cast<size_t>(n) < size - used ? static_cast<size_t>(n) : size - used - 1;
    }
}
}  // namespace

// From handle(), after decide(): only a sink session with a source partner negotiates; anything else ends it.
void SwitchPD::negotiate(uint16_t alertBits)
{
    const bool sinkSession =
        phase == SWITCH_PD_SINK && state.plugged && ((state.status1 & SWITCH_PD_STATUS1_SRC_MODE) == 0u);
    if (!sinkSession) {
        if (nego != Nego::Idle || state.pdSource || state.contractMillivolts)
            endNegotiation();
        if (phase == SWITCH_PD_SOURCE && state.plugged &&
            (alertBits & (SWITCH_PD_ALERT_CONTRACT | SWITCH_PD_ALERT_SRC_PLUGIN)))
            explicitContract = true;
        return;
    }
    if (alertBits & (SWITCH_PD_ALERT_CONTRACT | ALERT_PDO))
        state.pdSource = true;
    switch (nego) {
    case Nego::Idle:
    case Nego::NotPd:
        if (alertBits & SWITCH_PD_ALERT_CONTRACT) {
            if (alertBits & ALERT_PDO) {
                choose();
            } else {  // bm92t_event_handler: "In case of no new PDO, wait for it"
                nego = Nego::WaitPdos;
                negoTimer->setTimeoutMS(kPdoWaitMS);
            }
        } else if (nego == Nego::Idle && !(alertBits & SWITCH_PD_ALERT_PR_SWAP) &&
                   (state.status1 & SWITCH_PD_STATUS1_SPDSNK)) {
            choose();  // already sinking on a contract made before this boot (bm92txx first_init)
        }
        break;
    case Nego::WaitPdos:
        if (alertBits & ALERT_PDO)
            choose();
        break;
    case Nego::RdoSent:
        if ((alertBits & ALERT_CMD_DONE) && (state.status1 & STATUS1_LASTCMD_MASK)) {
            const unsigned last = (state.status1 & STATUS1_LASTCMD_MASK) >> STATUS1_LASTCMD_SHIFT;
            if (last == LASTCMD_ABORTED && ++failedRequests < kMaxFailedRequests) {
                // r8a: a MacBook's port aborts the first request at attach (the controller still busy with the
                // partner's own messages) and takes the same one a moment later: once more, kPdoWaitMS on.
                os_log(OS_LOG_DEFAULT, "SwitchPD: request aborted at attach (last command status 2): again in %u ms",
                       kPdoWaitMS);
                nego = Nego::WaitPdos;
                negoTimer->setTimeoutMS(kPdoWaitMS);
                break;
            }
            char why[64];
            snprintf(why, sizeof(why), "the request was refused (last command status %u)", last);
            fail(why);
        } else if (alertBits & SWITCH_PD_ALERT_CONTRACT) {
            verify();
        }
        break;
    case Nego::PsRdySent:
        if (alertBits & ALERT_CMD_DONE) {
#if DEVELOPMENT || DEBUG
            IOLog("SwitchPD: PS_RDY result %u (alert %#x status1 %#x status2 %#x)\n",
                  (state.status1 & STATUS1_LASTCMD_MASK) >> STATUS1_LASTCMD_SHIFT, alertBits, state.status1,
                  state.status2);
#endif
            if (state.status1 & STATUS1_LASTCMD_MASK) {
                fail("PS_RDY did not complete successfully");
                break;
            }
        }
        if (alertBits & SWITCH_PD_ALERT_CONTRACT) {
            withdraw();
            choose();
        } else if (alertBits & ALERT_CMD_DONE) {
            explicitContract = true;
            nego = Nego::Settling;
            negoTimer->setTimeoutMS(kPowerDelayMS);
        }
        break;
    case Nego::Settling:
    case Nego::Done:
    case Nego::Failed:
        // A contract we did not ask for (a multi-port charger re-balancing, a source reset): start over.
        if (alertBits & SWITCH_PD_ALERT_CONTRACT) {
            withdraw();
            if (nego == Nego::Failed && failedRequests >= kMaxFailedRequests)
                break;
            choose();
        }
        break;
    }
}

bool SwitchPD::readContract(uint32_t &pdo, uint32_t &rdo)
{
    uint8_t p[5] = {};
    uint8_t r[5] = {};
    if (tegra_i2c_read(kBus, kPD, CURRENT_PDO, p, sizeof(p)) || tegra_i2c_read(kBus, kPD, CURRENT_RDO, r, sizeof(r))) {
        return false;
    }
    if (p[0] != 4 || r[0] != 4)
        return false;
    pdo = le32(p + 1);
    rdo = le32(r + 1);
    return true;
}

// The source's PDOs, the choice, and the request unless the contract already is the one we would ask for.
void SwitchPD::choose()
{
    negoTimer->cancelTimeout();
    uint8_t raw[29] = {};
    if (tegra_i2c_read(kBus, kPD, READ_PDOS_SRC, raw, sizeof(raw))) {
        fail("the source's PDOs could not be read");
        return;
    }
    if (raw[0] > sizeof(raw) - 1 || (raw[0] & 3u)) {
        fail("invalid source PDO list length");
        return;
    }
    sourcePdoCount = raw[0] / 4;
    for (unsigned i = 0; i < sourcePdoCount; ++i)
        sourcePdos[i] = le32(raw + 1 + 4 * static_cast<size_t>(i));
    if (!sourcePdoCount) {
        if (nego != Nego::NotPd) {
            IOLog("SwitchPD: no USB-PD source capabilities: the charger keeps its own input detection (status1 %#x)\n",
                  state.status1);
        }
        explicitContract = false;
        nego = Nego::NotPd;
        return;
    }
    state.pdSource = true;
    choice = charge::choosePdo(sourcePdos, sourcePdoCount);
    if (!choice.position) {
        fail(choice.refusal);
        return;
    }
    if (!choice.inputLimit) {
        withdraw();
        if (switch_battery_prepare_pd_input(&state)) {
            fail("the charger input could not be disconnected for the zero-current contract");
            return;
        }
    }
    uint32_t pdo = 0;
    uint32_t rdo = 0;
    if ((state.status1 & SWITCH_PD_STATUS1_SPDSNK) && readContract(pdo, rdo) &&
        charge::contractMatches(choice, pdo, rdo)) {
        publish();  // ours already (made by an earlier boot): nothing to request
        return;
    }
    const uint32_t request = charge::encodeRdo(choice);
    const uint8_t message[5] = {4, static_cast<uint8_t>(request), static_cast<uint8_t>(request >> 8),
                                static_cast<uint8_t>(request >> 16), static_cast<uint8_t>(request >> 24)};
#if DEVELOPMENT || DEBUG
    IOLog("SwitchPD: request PDO %u %#x RDO %#x (alert %#x status1 %#x)\n", choice.position, choice.pdo, request,
          state.alert, state.status1);
#endif
    if (tegra_i2c_write(kBus, kPD, SET_RDO, message, sizeof(message)) || write16(COMMAND, SEND_RDO_CMD)) {
        fail("the request could not be written");
        return;
    }
    nego = Nego::RdoSent;
    negoTimer->setTimeoutMS(kReplyMS);
}

// ALERT_CONTRACT after our request: it must be the one asked for; then PS_RDY (bm92t_event_handler NEW_PDO).
void SwitchPD::verify()
{
    negoTimer->cancelTimeout();
    uint32_t pdo = 0;
    uint32_t rdo = 0;
    if (!readContract(pdo, rdo)) {
        fail("the new contract could not be read");
        return;
    }
    if (!charge::contractMatches(choice, pdo, rdo)) {
        char why[96];
        snprintf(why, sizeof(why), "the new contract is not the one requested (PDO %#x RDO %#x, asked RDO %#x)", pdo,
                 rdo, charge::encodeRdo(choice));
        fail(why);
        return;
    }
    if (write16(COMMAND, PS_RDY_CMD)) {
        fail("PS_RDY could not be written");
        return;
    }
    nego = Nego::PsRdySent;
    negoTimer->setTimeoutMS(kReplyMS);
}

// The contract becomes the input limit's (SwitchBattery, through notify()): one line.
void SwitchPD::publish()
{
    negoTimer->cancelTimeout();
    explicitContract = true;
    nego = Nego::Done;
    failedRequests = 0;
    state.contractMillivolts = static_cast<uint16_t>(choice.millivolts);
    state.inputLimitMilliamps = static_cast<uint16_t>(choice.inputLimit);
    char volts[12];
    char amps[12];
    char pdos[192];
    formatMilli(volts, sizeof(volts), choice.millivolts);
    formatMilli(amps, sizeof(amps), choice.inputLimit);
    formatPdos(pdos, sizeof(pdos), sourcePdos, sourcePdoCount, choice.position);
    IOLog("SwitchPD: contract %s V %s A (%s%s)\n", volts, amps, choice.dock ? "dock: " : "", pdos);
}

void SwitchPD::withdraw()
{
    state.contractMillivolts = 0;
    state.inputLimitMilliamps = 0;
}

// One line; the controller's own contract stays and nothing is published (the charger keeps its own detection).
void SwitchPD::fail(const char *why)
{
    negoTimer->cancelTimeout();
    withdraw();
    explicitContract = (state.status1 & SWITCH_PD_STATUS1_SPDSNK) != 0 && sourcePdoCount > 0;
    nego = Nego::Failed;
    ++failedRequests;
    char pdos[192];
    formatPdos(pdos, sizeof(pdos), sourcePdos, sourcePdoCount, 0);
    IOLog("SwitchPD: no USB-PD request: %s; the controller's own contract stays (source: %s)%s\n", why,
          sourcePdoCount ? pdos : "?",
          failedRequests >= kMaxFailedRequests ? "; no more requests until unplugged" : "");
}

void SwitchPD::endNegotiation()
{
    negoTimer->cancelTimeout();
    explicitContract = false;
    nego = Nego::Idle;
    failedRequests = 0;
    sourcePdoCount = 0;
    state.pdSource = false;
    withdraw();
}

void SwitchPD::negoFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchPD *>(owner);
    if (self->leaving)
        return;
    switch (self->nego) {
    case Nego::WaitPdos:
        self->choose();
        break;
    case Nego::RdoSent:
        self->fail("no contract within 1.5 s of the request");
        break;
    case Nego::PsRdySent:
        self->fail("PS_RDY was not completed within 1.5 s");
        break;
    case Nego::Settling:
        self->publish();
        break;
    default:
        return;
    }
    self->notifyAlt(0);
    self->notify();
}
