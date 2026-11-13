// SPDX-License-Identifier: BSD-2-Clause-Patent
// HW Test gpu.x1.dc_host1x_class, debug kernels only: does host1x route a class 0x70 stream to DC A on this unit, and
// does a WAIT_SYNCPT ahead of it hold the DC writes until the syncpoint is reached? If both, a producer's syncpoint
// could latch a flip with no CPU at all. L4T dc/dc_common.c drives DC_CMD_STATE_CONTROL through a host1x channel of
// NV_DISPLAY_CLASS_ID 0x70, T210's heads 0x10000 words apart (:39-41, :1043), with SETPAYLOAD and NONINCR_W (:58-63),
// opcodes T210's host1x lacks; this stream has SETCLASS, INCR and IMM only, and DC A is head 0.
//
// Writing debug.switch_display_dc_host1x_class runs it on the sysctl's thread, on NVJPG's host1x channel (idle while
// the HW Test runs), with three syncpoints of its own (gate, DC, channel); reading it gives the verdict line. Every
// stream is host1x's WAIT_SYNCPT on the gate, its body, then host1x's INCR_SYNCPT on the channel syncpoint, the job's
// fence. The setup is proven first: the CPU's write of DC_COM_SCRATCH_REGISTER_A (0x325) must read back, and a control
// stream with no body must wait for the CPU's increment of the gate and then run past. The probe's body is class
// 0x70's INCR of the scratch word with the complement of its value and IMM of DC_CMD_GENERAL_INCR_SYNCPT (0x000,
// IMMEDIATE) on the DC syncpoint. Nothing of it may land before the gate (held); after, the channel syncpoint says
// host1x ran past the class 0x70 words, the DC syncpoint and the scratch word say DC A took them. A channel that
// stopped is torn down and restarted, which drops its jobs (one of NVJPG's queued meanwhile too); the scratch word gets
// its value back. Included in the class body after NativeColumnScanDisplay.h.

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
static constexpr uint32_t kDcScratchA = 0x325, kDcGeneralIncrSyncpt = 0x000, kDcHost1xClass = 0x70;
static constexpr uint32_t kDcClassChannel = TEGRA_HOST1X_CHANNEL_NVJPG, kDcClassHeldUs = 2000, kDcClassRunUs = 50000;
static inline uint32_t dcClassSyncpts[3] = {};  // gate, DC, channel
static inline bool dcClassSyncptsReady = false, dcClassBusy = false;
static inline char dcClassLine[320] = "class70=not_run";
static inline struct sysctl_oid dcClassOid = {};

static int dcClassSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    if (req->newptr) {
        int value = 0;
        if (const int error = SYSCTL_IN(req, &value, sizeof(value)))
            return error;
        NativeLegacyDisplay *display = __atomic_load_n(&primaryDisplay, __ATOMIC_ACQUIRE);
        if (!display)
            return ENODEV;
        if (__atomic_exchange_n(&dcClassBusy, true, __ATOMIC_ACQUIRE))
            return EBUSY;
        display->dcClassProbe();
        __atomic_store_n(&dcClassBusy, false, __ATOMIC_RELEASE);
    }
    return SYSCTL_OUT(req, dcClassLine, strlen(dcClassLine) + 1);
}

static void dcClassRegister()
{
    if (dcClassOid.oid_name)
        return;
    switch_sysctl_oid(&dcClassOid, &sysctl__debug_children, "switch_display_dc_host1x_class",
                      static_cast<int>(CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED), dcClassSysctl,
                      "A", "write: run the host1x class 0x70 probe of DC A; read: its verdict");
    sysctl_register_oid(&dcClassOid);
}

// DC A's scratch word on the work loop (read, or written); false while SC7 has the controller unclocked.
bool dcClassScratch(uint32_t &value, bool write)
{
    struct Call {
        NativeLegacyDisplay *display;
        uint32_t *value;
        bool write, clocked;
    } call = {this, &value, write, false};

    getWorkLoop()->runAction(
        [](OSObject *, void *argument, void *, void *, void *) -> IOReturn {
            auto *c = static_cast<Call *>(argument);
            c->clocked = !c->display->hwSuspended;
            if (!c->clocked)
                return kIOReturnSuccess;
            if (c->write)
                c->display->dc[kDcScratchA] = *c->value;
            else
                *c->value = c->display->dc[kDcScratchA];
            return kIOReturnSuccess;
        },
        this, &call);
    return call.clocked;
}

bool dcClassScratchHolds(uint32_t before, uint32_t marker)
{
    uint32_t value = marker;
    dcClassScratch(value, true);
    dcClassScratch(value, false);
    const bool holds = value == marker;
    value = before;
    dcClassScratch(value, true);
    return holds;
}

bool dcClassSyncptsTaken()
{
    if (dcClassSyncptsReady)
        return true;
    for (uint32_t &id : dcClassSyncpts)
        if (tegra_host1x_syncpt_alloc(&id) != TEGRA_HOST1X_OK)
            return false;
    dcClassSyncptsReady = true;
    return true;
}

struct DcClassStream {
    uint32_t gateAt = 0, channelAt = 0;
    int error = 0;  // the submit's, or the gate increment's
    bool ran = false;
    char channel[200] = {};
};

// The gate's WAIT_SYNCPT, `body`, host1x's increment of the channel syncpoint; nothing before the gate.
bool dcClassSubmit(const uint32_t *body, uint32_t count, DcClassStream &s)
{
    s.gateAt = tegra_host1x_syncpt_read(dcClassSyncpts[0]) + 1;
    s.channelAt = tegra_host1x_syncpt_read(dcClassSyncpts[2]) + 1;
    uint32_t words[SWITCH_HOST1X_WAIT_WORDS + 8];
    uint32_t n = tegra_host1x_op_wait_syncpt(words, dcClassSyncpts[0], s.gateAt);
    for (uint32_t i = 0; i < count; ++i)
        words[n++] = body[i];
    words[n++] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0);
    words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, dcClassSyncpts[2]);
    s.error = switch_host1x_job_copy(kDcClassChannel, words, n, dcClassSyncpts[2], s.channelAt);
    return !s.error;
}

// The CPU raises the gate; then whether the channel ran past the stream. A stopped channel is torn down and restarted.
void dcClassRelease(DcClassStream &s)
{
    s.error = tegra_host1x_syncpt_release_failed(dcClassSyncpts[0], s.gateAt, 1);
    s.ran = !s.error && tegra_host1x_syncpt_wait(dcClassSyncpts[2], s.channelAt, kDcClassRunUs) == TEGRA_HOST1X_OK;
    tegra_host1x_describe_channel(kDcClassChannel, s.channel, sizeof(s.channel));
    if (!s.ran) {
        tegra_host1x_channel_teardown(kDcClassChannel);
        tegra_host1x_channel_restart(kDcClassChannel);
    }
}

void dcClassProbe()
{
    uint32_t before = 0;
    if (!getWorkLoop() || tegra_host1x_start() != TEGRA_HOST1X_OK || !dcClassSyncptsTaken() ||
        !dcClassScratch(before, false)) {
        snprintf(dcClassLine, sizeof(dcClassLine), "class70=not_run reason=%s",
                 getWorkLoop() && hwSuspended ? "suspended" : "host1x");
        return;
    }
    const uint32_t marker = ~before;
    const bool scratchHolds = dcClassScratchHolds(before, marker);

    DcClassStream control;
    if (!dcClassSubmit(nullptr, 0, control)) {
        snprintf(dcClassLine, sizeof(dcClassLine), "class70=not_run reason=submit_%d", control.error);
        return;
    }
    const bool controlHeld =
        tegra_host1x_syncpt_wait(dcClassSyncpts[2], control.channelAt, kDcClassHeldUs) == TEGRA_HOST1X_ETIMEDOUT;
    dcClassRelease(control);
    if (!controlHeld || !control.ran) {
        snprintf(dcClassLine, sizeof(dcClassLine), "class70=not_run reason=control held=%d gate=%d %s", controlHeld,
                 control.error, control.channel);
        IOLog("SwitchDisplay: host1x class 0x70 probe: %s\n", dcClassLine);
        return;
    }

    const uint32_t dcAt = tegra_host1x_syncpt_read(dcClassSyncpts[1]) + 1;
    const uint32_t body[] = {
        tegra_host1x_op_setclass(kDcHost1xClass, 0, 0),
        tegra_host1x_op_incr(kDcScratchA, 1),
        marker,
        tegra_host1x_op_imm(kDcGeneralIncrSyncpt, (TEGRA_HOST1X_COND_IMMEDIATE << 8) | dcClassSyncpts[1]),
    };
    DcClassStream probe;
    if (!dcClassSubmit(body, sizeof(body) / sizeof(body[0]), probe)) {
        snprintf(dcClassLine, sizeof(dcClassLine), "class70=not_run reason=submit_%d", probe.error);
        return;
    }
    const bool earlySyncpt = tegra_host1x_syncpt_wait(dcClassSyncpts[1], dcAt, kDcClassHeldUs) == TEGRA_HOST1X_OK;
    uint32_t early = before;
    dcClassScratch(early, false);
    const bool held = !earlySyncpt && early == before;
    dcClassRelease(probe);
    const bool incremented =
        earlySyncpt || tegra_host1x_syncpt_wait(dcClassSyncpts[1], dcAt, probe.ran ? kDcClassHeldUs : kDcClassRunUs) ==
                           TEGRA_HOST1X_OK;
    uint32_t after = before;
    dcClassScratch(after, false);
    uint32_t restore = before;
    dcClassScratch(restore, true);

    const bool written = scratchHolds ? after == marker : incremented;
    const char *verdict = probe.error                ? "not_run"
                          : !probe.ran               ? "faults"
                          : incremented && written   ? "routes"
                          : !incremented && !written ? "ignored"
                                                     : "partial";
    snprintf(dcClassLine, sizeof(dcClassLine), "class70=%s held=%d dc_syncpt=%d scratch=%s scratch_cpu=%s gate=%d %s",
             verdict, held, incremented,
             after == marker   ? "written"
             : after == before ? "unchanged"
                               : "other",
             scratchHolds ? "holds" : "not", probe.error, probe.channel);
    IOLog("SwitchDisplay: host1x class 0x70 probe: %s\n", dcClassLine);
}
#else
static void dcClassRegister()
{}
#endif
