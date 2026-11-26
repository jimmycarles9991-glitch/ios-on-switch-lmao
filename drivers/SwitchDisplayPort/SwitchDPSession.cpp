// SPDX-License-Identifier: GPL-2.0-only
// A hot-plugged sink, from its DPCD capabilities to a trained link scanning out (SwitchDisplayPortService.h): every
// step is the completion of the AUX transaction or the training wait before it. Order: Linux drivers/gpu/drm/tegra/sor.c
// tegra_sor_dp_enable and dp.c drm_dp_link_probe/_choose/_train; the display controller is running before SOR1 starts
// (the CRTC is enabled before the encoder in DRM's commit).
#include <IOKit/IOLib.h>

#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "SwitchDisplayPortService.h"


extern "C" void switch_displayport_audio_sink(const uint8_t *, unsigned, bool);

namespace {
constexpr uint8_t kDpcdD0 = 1;
constexpr unsigned kMaxEdidBlocks = 4;
}  // namespace

void SwitchDisplayPort::dispatch()
{
    switch (next) {
    case Next::Caps:
        if (switch_dp_caps_update(caps, request.data, request.address == SWITCH_DPCD_EXT_CAP_BASE)) {
            auxNative(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DPCD_EXT_CAP_BASE, nullptr, sizeof(caps), Next::Caps);
            break;
        }
        aux.setDownspread((caps[SWITCH_DPCD_MAX_DOWNSPREAD] & 1u) != 0);
        auxNative(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DPCD_SET_POWER, &kDpcdD0, 1, Next::Powered);
        break;
    case Next::Powered:
        edidBlock = 0;
        edidBlocks = 1;
        validEdidBlocks = 0;
        edidStep = 0;
        edidSteps = switch_dp_edid_requests(0, edidPlan);
        edidRead = 0;
        edidNext();
        break;
    case Next::Edid: {
        const struct switch_dp_aux_request &step = edidPlan[edidStep];
        if (step.op == SWITCH_DP_AUX_I2C_MOT_READ) {
            for (unsigned i = 0; i < 16; ++i) {
                edid[edidBlock * 128 + edidRead * 16 + i] = request.data[i];
            }
            ++edidRead;
        }
        ++edidStep;
        edidNext();
        break;
    }
    case Next::LinkSet: {
        const uint8_t coding[2] = {static_cast<uint8_t>((caps[SWITCH_DPCD_MAX_DOWNSPREAD] & 1u) << 4), 0x01};
        auxNative(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DPCD_DOWNSPREAD_CTRL, coding, 2, Next::Coding);
        break;
    }
    case Next::Coding:
        sor.enableLink(tu);
        trainOut = switch_dp_train_start(&train, link.lanes,
                                         link.rate == SWITCH_DP_RATE_HBR2 && (caps[2] & SWITCH_DPCD_TPS3) != 0,
                                         caps[SWITCH_DPCD_REV], caps[SWITCH_DPCD_TRAINING_AUX_RD_INTERVAL]);
        applyTraining();
        break;
    case Next::TrainingSet:
        if (train.tps3) {
            uint8_t levels[2];
            switch_dp_train_set2(&train, levels);
            auxNative(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DPCD_TRAINING_LANE0_1_SET2, levels, (train.lanes + 1) / 2,
                      Next::PostcursorSet);
            break;
        }
        [[fallthrough]];
    case Next::PostcursorSet:
        stepTimer->setTimeoutUS(trainOut.wait_us);
        break;
    case Next::TrainingStatus:
        trainOut = switch_dp_train_status(&train, request.data);
        applyTraining();
        break;
    case Next::TrainingStopped:
        teardownLink();
        chooseAndTrain(true);
        break;
    case Next::Trained:
        trained();
        break;
    case Next::Cec:
        cecComplete(true);
        break;
    case Next::LinkStatus:
        if ((request.data[1] & 0x40u) && cec.enabled)
            cec.irq = true;
        if (stage == Stage::Running && !switch_dp_link_holds(request.data + 2, link.lanes)) {
            IOLog("SwitchDisplayPort: IRQ_HPD: the link lost lock (lanes %#x %#x align %#x); training again\n",
                  request.data[2], request.data[3], request.data[4]);
            teardownLink();
            chooseAndTrain();
        }
        break;
    }
}

void SwitchDisplayPort::edidNext()
{
    if (edidStep < edidSteps) {
        auxStart(edidPlan[edidStep], Next::Edid);
        return;
    }
    if (switch_dp_edid_block_modes(edid + edidBlock * 128u, edidBlock == 0, &modes))
        validEdidBlocks |= 1u << edidBlock;
    if (edidBlock == 0 && validEdidBlocks) {
        edidBlocks = 1u + edid[126];
        edidBlocks = edidBlocks > kMaxEdidBlocks ? kMaxEdidBlocks : edidBlocks;
    }
    if (++edidBlock < edidBlocks) {
        edidStep = 0;
        edidRead = 0;
        edidSteps = switch_dp_edid_requests(edidBlock, edidPlan);
        edidNext();
        return;
    }
    edidDone();
}

void SwitchDisplayPort::edidDone()
{
    maxRate = switch_dp_rate_cap(caps[1]);
    const uint8_t sinkLanes = caps[2] & 0x1fu;
    maxLanes = sinkLanes < altLanes ? sinkLanes : (uint8_t)altLanes;
    const bool enhanced = (caps[2] & SWITCH_DPCD_ENHANCED_FRAME) != 0;
    const bool spread = (caps[SWITCH_DPCD_MAX_DOWNSPREAD] & 1u) != 0;
    const auto refKhz = TegraSor1::referenceClockKhz();
    const bool b01 = switch_car_soc_t210b01();
    unsigned admitted = 0;
    for (unsigned i = 0; i < modes.count; ++i) {
        const auto &candidate = modes.modes[i];
        switch_dp_plld2 pll;
        switch_dp_tu transport;
        switch_dp_link choice;
        unsigned cursor = 0;
        if (switch_dp_mode_allowed(&candidate, switch_dp_payload_kbps(maxRate, maxLanes)) &&
            switch_dp_plld2_for(candidate.pixel_khz, refKhz, b01, &pll) &&
            switch_dp_next_link(candidate.pixel_khz, candidate.hactive, switch_dp_mode_htotal(&candidate),
                                maxRate, maxLanes, enhanced, spread, &cursor, &choice, &transport))
            modes.modes[admitted++] = candidate;
    }
    modes.count = admitted;
    IOLog("SwitchDisplayPort: sink DPCD %u.%u, %u lanes at up to %u.%02u Gb/s, EDID %u block%s, %u modes\n",
          caps[0] >> 4, caps[0] & 0xfu, sinkLanes, switch_dp_rate_khz(maxRate) / 100000,
          (switch_dp_rate_khz(maxRate) / 1000) % 100, edidBlocks, edidBlocks == 1 ? "" : "s", modes.count);
    stage = Stage::Ready;
    cecEdid();
    if (!screen->publish(modes, edid, edidBlocks * 128u))
        fail("the external display", "could not publish identity and timings");
}

void SwitchDisplayPort::chooseAndTrain(bool nextChoice)
{
    if (!nextChoice)
        linkCursor = 0;
    if (wantedMode < 0 || static_cast<unsigned>(wantedMode) >= modes.count) {
        fail("the mode", "the requested timing is unavailable");
        return;
    }
    modeIndex = static_cast<unsigned>(wantedMode);
    mode = modes.modes[modeIndex];
    const bool enhanced = (caps[2] & SWITCH_DPCD_ENHANCED_FRAME) != 0;
    const bool spread = (caps[SWITCH_DPCD_MAX_DOWNSPREAD] & 1u) != 0;
    if (!switch_dp_next_link(mode.pixel_khz, mode.hactive, switch_dp_mode_htotal(&mode), maxRate, maxLanes,
                             enhanced, spread, &linkCursor, &link, &tu)) {
        fail("the link", "the available rate and lane combinations were exhausted");
        return;
    }
    stage = Stage::Training;
    char why[128] = "";
    if (!sorUp) {
        if (!sor.powerUp(why, sizeof(why))) {
            fail("SOR1", why);
            return;
        }
        sorUp = true;
    }
    if (!sor.pixelClock(mode.pixel_khz, why, sizeof(why))) {
        fail("the pixel clock", why);
        return;
    }
    if (!dcbUp) {
        if (!dcb.powerUp(why, sizeof(why))) {
            fail("display controller B", why);
            return;
        }
        dcbUp = true;
        dcSource->enable();
    }
    dcb.setMode(mode);
    if (!sor.startLink(link, enhanced, why, sizeof(why))) {
        fail("SOR1", why);
        return;
    }
    const uint8_t linkSet[2] = {link.rate, (uint8_t)(link.lanes | (enhanced ? SWITCH_DPCD_ENHANCED_FRAME : 0u))};
    auxNative(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DPCD_LINK_BW_SET, linkSet, 2, Next::LinkSet);
}

void SwitchDisplayPort::applyTraining()
{
    switch (trainOut.act) {
    case SWITCH_DP_TRAIN_DRIVE:
        sor.drive(train, trainOut);
        auxNative(SWITCH_DP_AUX_NATIVE_WRITE, trainOut.dpcd_addr, trainOut.dpcd, trainOut.dpcd_len,
                  Next::TrainingSet);
        break;
    case SWITCH_DP_TRAIN_DONE:
        sor.drive(train, trainOut);
        auxNative(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DPCD_TRAINING_PATTERN_SET, trainOut.dpcd, 1, Next::Trained);
        break;
    case SWITCH_DP_TRAIN_FAIL: {
#if DEVELOPMENT || DEBUG
        sor.logTrainingState();
        IOLog("SwitchDisplayPort: training DPCD interval %#x, status %02x %02x %02x %02x, adjust %02x %02x\n",
              caps[SWITCH_DPCD_TRAINING_AUX_RD_INTERVAL], request.data[0], request.data[1], request.data[2],
              request.data[3], request.data[4], request.data[5]);
        IOLog("SwitchDisplayPort: training at %u Mb/s x%u: %s\n", switch_dp_rate_khz(link.rate) / 100, link.lanes,
              train.why != nullptr ? train.why : "failed");
#endif
        sor.drive(train, trainOut);
        auxNative(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DPCD_TRAINING_PATTERN_SET, trainOut.dpcd, 1,
                  Next::TrainingStopped);
        break;
    }
    }
}

void SwitchDisplayPort::trained()
{
    char why[128] = "";
    if (!sor.attach(mode, tu, why, sizeof(why))) {
        fail("SOR1", why);
        return;
    }
    dcb.enableSor(true);
    if (!sor.wake(why, sizeof(why))) {
        fail("SOR1", why);
        return;
    }
    stage = Stage::Running;
    IOLog("SwitchDisplayPort: link up at %u Mb/s x%u (TU %u, watermark %u)\n", switch_dp_rate_khz(link.rate) / 100,
          link.lanes, tu.tu_size, tu.watermark);
    screen->linkUp(mode, &dcb);
    wakeModeWaiters(false);
    switch_displayport_audio_sink(edid, validEdidBlocks, true);
    scheduleAux();
}
