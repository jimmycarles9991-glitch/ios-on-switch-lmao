/* Root-boot observations only. Stage numbers never authorize storage or exec. */
volatile uint64_t switch_root_record[24];
static unsigned root_boot_enabled;
static uint64_t root_last_draw;

static void root_screen(void)
{
    if (!root_boot_enabled || !switch_diag_record.ready || switch_diag_record.panicked)
        return;
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    if (switch_diag_console_active()) {
        char line[105];
        for (unsigned y = 120; y < 180; y++) {
            for (unsigned x = 16; x < 1264; x++)
                switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
        }
        (void)snprintf(line, sizeof(line), "ROOT %llu ERROR %llx AUTH %llu OWNER %llu CACHE %llx DATA %llx",
                       switch_root_record[2], switch_root_record[3], switch_root_record[8], switch_root_record[9],
                       switch_root_record[11], switch_root_record[12]);
        text(120, line, sizeof(line), switch_root_record[3] ? SWITCH_RED : SWITCH_GREEN);
        (void)snprintf(line, sizeof(line), "POLICY %llu/%llu VERIFIED SD %llu", switch_root_record[5],
                       switch_root_record[6], switch_root_record[7]);
        text(140, line, sizeof(line), SWITCH_WHITE);
        (void)snprintf(line, sizeof(line), "PID1 CALLS %llu EXEC %llx ROOT OWNER %llx MOBILE %llx",
                       switch_root_record[15], switch_root_record[13], switch_root_record[19], switch_root_record[20]);
        text(160, line, sizeof(line), SWITCH_WHITE);
        console_flush();
        sync_pixels();
        (void)ml_set_interrupts_enabled(previous);
        return;
    }
    for (unsigned y = 340; y < 660; y++) {
        for (unsigned x = 16; x < 1264; x++)
            switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
    }
    const uint64_t stage = switch_root_record[2];
    static const char *const stages[] = {
        "NATIVE ROOT BOOT IN PROGRESS",
        "QUALIFYING NATIVE READ ONLY STORAGE",
        "LOADING SIGNED ROOT POLICY",
        "AUTHENTICATING ROOT POLICY",
        "ROOT POLICIES AUTHENTICATED - PUBLISHING",
        "VERIFIED STORAGE READY - FINDING ROOT",
        "MOUNTING ORIGINAL APFS ROOT",
        "ORIGINAL APFS ROOT MOUNTED",
        "OWNER ROOT POLICY DECISION RECORDED",
        "PREPARING ORIGINAL SYSTEMOS CACHE",
        "SYSTEMOS CACHE PREPARATION RETURNED",
        "MOUNTING DISPOSABLE RAM DATA",
        "RAM DATA PREPARATION RETURNED",
        "ORIGINAL LAUNCHD EXEC RETURNED",
        "ORIGINAL PID1 SYSCALL OBSERVED",
    };
    const char *message = stages[0];
    if (switch_root_record[3]) {
        message = "NATIVE ROOT BOOT REFUSED OR FAILED";
    } else if (stage < sizeof(stages) / sizeof(stages[0])) {
        message = stages[stage];
    }
    text(360, message, 104, switch_root_record[3] ? SWITCH_RED : SWITCH_GREEN);
    hex_line(400, "ROOT STAGE / ERROR=", (stage << 32) | switch_root_record[3]);
    hex_line(420, "POLICY DISK=", switch_root_record[4]);
    hex_line(440, "POLICY LOADED BYTES=", switch_root_record[5]);
    hex_line(460, "POLICY TOTAL BYTES=", switch_root_record[6]);
    hex_line(480, "AUTHENTICATED SD BYTES=", switch_root_record[7]);
    hex_line(500, "APPLE ROOT AUTH / OWNER=", (switch_root_record[8] << 32) | switch_root_record[9]);
    hex_line(520, "MOUNTED ROOT DEVICE=", switch_root_record[10]);
    hex_line(540, "CACHE / DATA RESULT=", (switch_root_record[11] << 32) | switch_root_record[12]);
    if (stage == 12) {
        hex_line(560, "DATA ROOT UID / GID=", switch_root_record[19]);
        hex_line(580, "DATA MOBILE UID / GID=", switch_root_record[20]);
        hex_line(600, "DATA MOUNT FLAGS=", switch_root_record[21]);
    } else {
        hex_line(560, "ORIGINAL LAUNCHD EXEC RESULT=", switch_root_record[13]);
        hex_line(580, "FIRST PID1 SYSCALL PC=", switch_root_record[14]);
        hex_line(600, "PID1 SYSCALL COUNT=", switch_root_record[15]);
    }
    hex_line(620, "LAST SYSCALL / ERROR=", (switch_root_record[16] << 32) | switch_root_record[17]);
    text(640, "OS READ ONLY - DATA WRITES STAY IN RAM", 104, SWITCH_WHITE);
    sync_pixels();
    (void)ml_set_interrupts_enabled(previous);
}

void switch_root_progress(unsigned stage, unsigned disk, uint64_t done, uint64_t total)
{
    if (stage & 0x80000000u) {
        switch_root_record[3] = stage & 0x7fffffffu;
    } else {
        switch_root_record[2] = stage;
    }
    switch_root_record[4] = disk;
    switch_root_record[5] = done;
    switch_root_record[6] = total;
    root_screen();
}

void switch_root_io_progress(uint64_t bytes, unsigned error)
{
    switch_root_record[7] = bytes;
    if (error && !switch_root_record[3])
        switch_root_record[3] = 500 + error;
    uint64_t now = 0;
    uint64_t freq = 0;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(now));
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(freq));
    if (error || (freq && now - root_last_draw >= freq)) {
        root_last_draw = now;
        root_screen();
    }
}

void switch_root_data_ownership(uint64_t flags, uint64_t root, uint64_t mobile)
{
    switch_root_record[19] = root;
    switch_root_record[20] = mobile;
    switch_root_record[21] = flags;
}

void switch_root_event(unsigned stage, int error, uint64_t detail)
{
    switch_root_record[2] = stage;
    if (stage == 8) {
        switch_root_record[8] = (unsigned)error;
        switch_root_record[9] = detail & 1;
        switch_root_record[10] = detail >> 32;
    } else {
        if (error && !switch_root_record[3])
            switch_root_record[3] = (stage << 16) | (unsigned)error;
        if (stage == 10)
            switch_root_record[11] = (unsigned)error;
        if (stage == 12)
            switch_root_record[12] = (unsigned)error;
        if (stage == 13) {
            switch_root_record[13] = (unsigned)error;
            switch_root_record[18] = error == 0;
        }
    }
    root_screen();
}

static void root_syscall(unsigned code, int error, int pid, uint64_t pc, uint64_t cpsr)
{
    if (!root_boot_enabled || !switch_root_record[18] || pid != 1 || (cpsr & 31))
        return;
    if (!switch_root_record[15])
        switch_root_record[14] = pc;
    switch_root_record[15]++;
    switch_root_record[16] = code;
    switch_root_record[17] = (unsigned)error;
    switch_root_record[2] = 14;
    if (switch_root_record[15] == 1)
        switch_diag_stage("original PID1 EL0 syscall observed");
    if (switch_root_record[15] == 1 || error)
        root_screen();
}

void switch_root_record_begin(void)
{
    root_boot_enabled = 1;
    switch_root_record[0] = UINT64_C(0x53574348524f4f54);
    switch_root_record[1] = 1;
    switch_root_record[8] = switch_root_record[11] = switch_root_record[12] = switch_root_record[13] = UINT32_MAX;
    switch_root_record[19] = switch_root_record[20] = UINT64_MAX;
}

void switch_root_record_stage(unsigned stage)
{
    switch_root_record[2] = stage;
}
