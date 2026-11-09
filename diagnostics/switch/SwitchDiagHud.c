/* Deliberately small source-only diagnostic for the one-CPU native experiment, debug kernels only.
 * Compile-time + explicit boot-argument gated; no firmware or security services.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <kern/misc_protos.h>
#include <mach/vm_param.h>
#include "SwitchUserFixture.h"
#include "SwitchSD.h"
#include "SwitchFiles.h"
#include "storage/block_backend.h"
#include "SwitchBlockExpectations.h"
#include <libkern/crypto/sha2.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/boot.h>
#include "SwitchDiag.h"
#include "SwitchDiagDisplay.h"
#include "SwitchDiagHud.h"

#if DEVELOPMENT || DEBUG
extern int vsnprintf(char *, size_t, const char *, va_list);
extern void panic_spin_forever(void) __attribute__((noreturn));

static volatile uint32_t *framebuffer;
/* Fixed RAM text history. No allocation, firmware, UART or storage access. */
#define CONSOLE_ROWS    24u
#define CONSOLE_COLUMNS 104u
static char console_lines[CONSOLE_ROWS][CONSOLE_COLUMNS + 1];
static unsigned console_enabled, console_busy, console_head, console_column, console_used;
static uint64_t console_last_draw;
static unsigned hud_hidden;
/* magic,armed,step,error,PC,CPSR,pid,last-return,getpid,sysctl,flags1,flags2,page-size,old-length */
volatile uint64_t switch_user_record[14];

static void sync_pixels(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static void text(unsigned y, const char *message, unsigned limit, uint32_t color)
{
    if (!framebuffer || switch_native_text_console_active())
        return;
    (void)switch_diag_text(framebuffer, 16, y, 704, message, limit, color);
}

static void hex_line(unsigned y, const char *label, uint64_t value)
{
    char line[64];
    unsigned n = 0;
    while (label[n] && n < sizeof(line) - 19) {
        line[n] = label[n];
        ++n;
    }
    line[n++] = '0';
    line[n++] = 'X';
    for (unsigned i = 0; i < 16; ++i) {
        unsigned digit = (unsigned)((value >> (60 - 4 * i)) & 15);
        line[n++] = "0123456789ABCDEF"[digit];
    }
    line[n] = 0;
    text(y, line, sizeof(line), SWITCH_WHITE);
}

int switch_diag_console_active(void)
{
    return console_enabled && switch_diag_record.ready && !switch_diag_record.panicked;
}

void switch_diag_graphics_takeover(void)
{
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    if (!switch_diag_record.panicked && framebuffer) {
        hud_hidden = 1;
        switch_diag_record.ready = 0;
    }
    (void)ml_set_interrupts_enabled(previous);
}

void switch_diag_graphics_release(void)
{
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    if (hud_hidden && !switch_diag_record.panicked) {
        hud_hidden = 0;
        switch_diag_record.ready = 1;
    }
    (void)ml_set_interrupts_enabled(previous);
}

static void console_blank(unsigned row)
{
    for (unsigned i = 0; i < CONSOLE_COLUMNS; i++)
        console_lines[row][i] = ' ';
    console_lines[row][CONSOLE_COLUMNS] = 0;
}

/* Called with interrupts disabled on the already-required single CPU. */
static void console_flush(void)
{
    uint64_t now = 0;
    uint64_t freq = 0;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(now));
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(freq));
    if (console_last_draw && freq && now - console_last_draw < freq / 5)
        return;
    console_last_draw = now;
    unsigned first = console_used == CONSOLE_ROWS ? (console_head + 1) % CONSOLE_ROWS : 0;
    for (unsigned row = 0; row < CONSOLE_ROWS; row++) {
        const char *line = row < console_used ? console_lines[(first + row) % CONSOLE_ROWS] : NULL;
        if (line) {
            text(180 + row * 20, line, CONSOLE_COLUMNS + 1, SWITCH_WHITE);
        } else {
            for (unsigned y = 180 + row * 20; y < 194 + row * 20; y++) {
                for (unsigned x = 16; x < 1264; x++)
                    switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
            }
        }
    }
    sync_pixels();
}

void switch_diag_console_putc(char c)
{
    if (!switch_diag_console_active())
        return;
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    if (console_busy) {
        (void)ml_set_interrupts_enabled(previous);
        return;
    }
    console_busy = 1;
    if (c != '\r') {
        if (c == '\n' || console_column == CONSOLE_COLUMNS) {
            console_head = (console_head + 1) % CONSOLE_ROWS;
            if (console_used < CONSOLE_ROWS)
                console_used++;
            console_blank(console_head);
            console_column = 0;
            console_flush();
        }
        if (c != '\n') {
            if (c == '\b') {
                if (console_column)
                    console_lines[console_head][--console_column] = ' ';
            } else {
                char shown = (unsigned char)c >= 32 && (unsigned char)c < 127 ? c : '?';
                if (c == '\t')
                    shown = ' ';
                console_lines[console_head][console_column++] = shown;
            }
        }
    }
    console_busy = 0;
    (void)ml_set_interrupts_enabled(previous);
}

void switch_diag_process_event(int pid, const char *name, const char *event, int status)
{
    (void)pid;
    (void)name;
    (void)event;
    (void)status;
}

void switch_diag_hud_init(const boot_args *args, int cpus)
{
    if (cpus != 1)
        return; /* Four-core native uses only the stock video console. */
    if (!switch_diag_video_matches(args->Video.v_baseAddr, args->Video.v_display, args->Video.v_rowBytes,
                                   args->Video.v_width, args->Video.v_height, args->Video.v_depth))
        return;
    /* io_map's bootstrap path is available once arm_vm_init returned. */
    vm_offset_t mapped = ml_io_map((vm_offset_t)SWITCH_FB_PA, (vm_size_t)SWITCH_FB_BYTES);
    if (!mapped)
        return;
    framebuffer = (volatile uint32_t *)mapped;
    switch_diag_record.magic = UINT64_C(0x53574348584e5531); /* SWCHXNU1 */
    switch_diag_record.framebuffer_va = mapped;
    switch_diag_record.ready = 1;
    switch_diag_clear(framebuffer);
    text(24, "SWITCH NATIVE XNU DIAGNOSTICS", 104, SWITCH_WHITE);
    text(64, "XNU VM INITIALIZED", 104, SWITCH_GREEN);
    text(680, "KERNEL DIAGNOSTICS ONLY - USERSPACE IS NOT ESTABLISHED", 104, SWITCH_WHITE);
    switch_diag_stage("arm_vm_init returned");
}

void switch_diag_stage(const char *message)
{
    if (!switch_diag_record.ready || switch_diag_record.panicked)
        return;
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    unsigned n = 0;
    for (; message[n] && n < sizeof(switch_diag_record.stage) - 1; ++n)
        switch_diag_record.stage[n] = message[n];
    switch_diag_record.stage[n] = 0;
    ++switch_diag_record.stages;
    for (unsigned y = 100; y < 120; ++y) {
        for (unsigned x = 16; x < 1264; ++x)
            switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
    }
    text(100, switch_diag_record.stage, sizeof(switch_diag_record.stage), SWITCH_GREEN);
    sync_pixels();
    (void)ml_set_interrupts_enabled(previous);
}

__attribute__((no_stack_protector)) void switch_diag_hud_panic(const char *format, va_list *args, unsigned long caller)
{
    if (hud_hidden && framebuffer) {
        hud_hidden = 0;
        switch_diag_record.ready = 1;
    }
    if (!switch_diag_record.ready)
        return;
    __asm__ volatile("msr daifset, #15" ::: "memory");
    /* One CPU is required at init. A recursive formatter fault must not redraw. */
    if (switch_diag_record.panicked)
        panic_spin_forever();
    switch_diag_record.panicked = 1;
    for (unsigned y = 140; y < 660; y++) {
        for (unsigned x = 16; x < 1264; x++)
            switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
    }
    switch_diag_record.caller = caller;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(switch_diag_record.current_el));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(switch_diag_record.sctlr));
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(switch_diag_record.tcr));
    __asm__ volatile("mrs %0, esr_el1" : "=r"(switch_diag_record.sampled_esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(switch_diag_record.sampled_far));
    /* Draw the header before formatting so a bad panic argument cannot leave a success banner. */
    text(144, "XNU PANIC - EXECUTION STOPPED", 104, SWITCH_RED);
    hex_line(184, "CALLER=", caller);
    hex_line(204, "CURRENTEL=", switch_diag_record.current_el);
    hex_line(224, "SCTLR=", switch_diag_record.sctlr);
    hex_line(244, "TCR=", switch_diag_record.tcr);
    hex_line(264, "SAMPLED ESR=", switch_diag_record.sampled_esr);
    hex_line(284, "SAMPLED FAR=", switch_diag_record.sampled_far);
    sync_pixels();
    /* NOLINTNEXTLINE(cppcoreguidelines-init-variables): va_copy initializes it */
    va_list copy;
    va_copy(copy, *args);
    int count = vsnprintf(switch_diag_record.panic_text, sizeof(switch_diag_record.panic_text), format, copy);
    va_end(copy);
    switch_diag_record.truncated = count < 0 || (unsigned)count >= sizeof(switch_diag_record.panic_text);
    if (switch_diag_text(framebuffer, 16, 324, 628, switch_diag_record.panic_text,
                         sizeof(switch_diag_record.panic_text), SWITCH_WHITE))
        switch_diag_record.truncated = 1;
    if (switch_diag_record.truncated)
        text(644, "PANIC TEXT TRUNCATED", 104, SWITCH_RED);
    for (unsigned y = 680; y < 700; ++y) {
        for (unsigned x = 16; x < 1264; ++x)
            switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
    }
    text(680, "HALTED ON PANIC - PHOTOGRAPH THIS SCREEN", 104, SWITCH_RED);
    sync_pixels();
    panic_spin_forever();
}

/* Observe the normal syscall implementation; no result, credentials or
 * authentication decision is changed. Exact PCs and buffers name only the
 * separately signed own fixture. A third EL0 syscall proves the first two
 * returned and userspace stored their real results. */
static void files_digest_line(unsigned y, const char *label, const uint8_t digest[32])
{
    char line[104];
    unsigned at = 0;
    while (label[at] && at < 32) {
        line[at] = label[at];
        at++;
    }
    for (unsigned i = 0; i < 32; i++) {
        line[at++] = "0123456789ABCDEF"[digest[i] >> 4];
        line[at++] = "0123456789ABCDEF"[digest[i] & 15];
    }
    line[at] = 0;
    text(y, line, 104, SWITCH_WHITE);
}

void switch_diag_files_update(void)
{
    if (!switch_diag_record.ready || switch_diag_record.panicked || switch_files_record[0] != SWITCH_FILES_MAGIC)
        return;
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    for (unsigned y = 350; y < 660; y++) {
        for (unsigned x = 16; x < 1264; x++)
            switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
    }
    const char *message = "READ-ONLY FAT FILE CHECK IN PROGRESS";
    if (switch_files_record[3]) {
        message = "FILE CHECK REFUSED OR FAILED - NO RETRY";
    } else if (switch_files_record[2] == 9) {
        message = "FAT FILE SAMPLES VERIFIED - READ ONLY";
    }
    text(360, message, 104, switch_files_record[2] == 9 && !switch_files_record[3] ? SWITCH_GREEN : SWITCH_WHITE);
    hex_line(400, "FILE STAGE / ERROR=", (switch_files_record[2] << 32) | switch_files_record[3]);
    if (switch_files_record[32]) {
        hex_line(420, "GPT ERROR=", switch_files_record[32]);
    } else {
        hex_line(420, "FAT ERROR / SD ERROR=", (switch_files_record[4] << 32) | switch_files_record[5]);
    }
    hex_line(440, "CARD CAPACITY SECTORS=", switch_files_record[6]);
    if (switch_files_record[32]) {
        hex_line(460, "GPT ENTRY COUNT / BYTES=", (switch_files_record[35] << 32) | (uint32_t)switch_files_record[36]);
    } else {
        hex_line(460, "FAT PARTITION FIRST=", switch_files_record[7]);
    }
    hex_line(480, "FAT PARTITION SECTORS=", switch_files_record[8]);
    hex_line(500, "SHA SELFTEST / CLUSTER BYTES=", (switch_files_record[29] << 32) | switch_files_record[9]);
    hex_line(520, "FIXTURE MATCH / HASHED BYTES=", (switch_files_record[17] << 32) | switch_files_record[16]);
    hex_line(540, "SPLIT VOLUMES / SAMPLES=", (switch_files_record[18] << 32) | switch_files_record[19]);
    hex_line(560, "CURRENT VOLUME / SAMPLE=", (switch_files_record[21] << 32) | (uint32_t)switch_files_record[20]);
    hex_line(580, "CURRENT SAMPLE OFFSET=", switch_files_record[22]);
    if (switch_files_record[3] == 208 || switch_files_record[3] == 211) {
        files_digest_line(600, "ACTUAL SHA256=", switch_files_digest);
        files_digest_line(620, "EXPECTED SHA256=", switch_files_expected_digest);
    } else {
        hex_line(600, "SECTORS READ / SD STAGE=", (switch_files_record[10] << 32) | switch_files_record[13]);
        hex_line(620, "LAST LBA=", switch_files_record[11]);
    }
    hex_line(640, "VERIFIED FILE BYTES=", switch_files_record[24]);
    sync_pixels();
    (void)ml_set_interrupts_enabled(previous);
}

#include "SwitchBlockDiag.h"
#if defined(XNU_SWITCH_VERIFIED_ROOT) && XNU_SWITCH_VERIFIED_ROOT
#include "SwitchRootDiag.h"
#endif

void switch_diag_user_syscall(unsigned code, int error, int pid, uint64_t pc, uint64_t cpsr, int result)
{
#if defined(XNU_SWITCH_VERIFIED_ROOT) && XNU_SWITCH_VERIFIED_ROOT
    root_syscall(code, error, pid, pc, cpsr);
#endif
    if (!switch_diag_record.ready || switch_diag_record.panicked || !switch_user_record[1] || switch_user_record[3] ||
        (cpsr & 0x1f) != 0 || pid != 1)
        return;
    if (switch_user_record[2] == 3) {
        block_checkpoint(pc, cpsr, pid, code, error, result);
        return;
    }
    unsigned step = (unsigned)switch_user_record[2];
    if ((step == 0 && (code != 20 || pc != SWITCH_USER_AFTER_GETPID)) ||
        (step == 1 && (code != 202 || pc != SWITCH_USER_AFTER_SYSCTL)) ||
        (step == 2 && (code != 20 || pc != SWITCH_USER_CHECKPOINT)) || step > 2)
        return;
    switch_user_record[4] = pc;
    switch_user_record[5] = cpsr;
    switch_user_record[6] = (uint64_t)pid;
    switch_user_record[7] = (uint64_t)result;
    if (error || result != (step == 1 ? 0 : 1)) {
        switch_user_record[3] = 0x100 + (unsigned)error;
        return;
    }
    if (step == 2) {
        uint64_t results[4];
        uint64_t page = 0;
        uint64_t length = 0;
        if (copyin(SWITCH_USER_RESULTS, results, sizeof(results)) ||
            copyin(SWITCH_USER_USER_PAGE_SIZE, &page, sizeof(page)) ||
            copyin(SWITCH_USER_OLD_LENGTH, &length, sizeof(length))) {
            switch_user_record[3] = 0x200;
            return;
        }
        for (unsigned i = 0; i < 4; ++i)
            switch_user_record[8 + i] = results[i];
        switch_user_record[12] = page;
        switch_user_record[13] = length;
        if (results[0] != 1 || results[1] != 0 || (results[2] & (1u << 29)) || (results[3] & (1u << 29)) ||
            page != (uint64_t)PAGE_SIZE || length != 4) {
            switch_user_record[3] = 0x300;
            return;
        }
    }
    switch_user_record[2] = step + 1;
    if (step == 2)
        switch_diag_stage("SIGNED PID1 TEST PROCESS RUNNING");
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    for (unsigned y = 144; y < 164; ++y) {
        for (unsigned x = 16; x < 1264; ++x)
            switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
    }
    const char *reached = "EL0 USERSPACE VERIFIED - PID 1";
    if (step == 0) {
        reached = "EL0 ENTERED - GETPID RETURNED 1";
    } else if (step == 1) {
        reached = "EL0 SYSCTL COMPLETED - WAITING FOR USER ACKNOWLEDGEMENT";
    }
    text(144, reached, 104, SWITCH_GREEN);
    hex_line(184, "USER PC=", pc);
    hex_line(204, "USER CPSR=", cpsr);
    hex_line(224, "COMPLETED TEST SYSCALLS=", step + 1);
    if (step == 2) {
        hex_line(264, "GETPID RESULT=", switch_user_record[8]);
        hex_line(284, "SYSCTL RESULT=", switch_user_record[9]);
        hex_line(304, "USER PAGE SIZE=", switch_user_record[12]);
        for (unsigned y = 680; y < 700; ++y) {
            for (unsigned x = 16; x < 1264; ++x)
                switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
        }
        text(680, "OWN SIGNED TEST PROCESS - IPADOS APPS ARE NOT RUNNING", 104, SWITCH_WHITE);
    }
    sync_pixels();
    (void)ml_set_interrupts_enabled(previous);
    if (step == 2) {
        switch_sd_probe();
        block_start();
        if (switch_sd_record[0] == SWITCH_SD_DIAG_MAGIC && switch_files_record[0] != SWITCH_FILES_MAGIC) {
            previous = ml_set_interrupts_enabled(FALSE);
            const char *message = "SD REFUSED OR FAILED - NO RETRY";
            if (switch_sd_record[2] == 5) {
                message = "SD SECTOR 0 MATCHES U-BOOT - READ ONLY";
            } else if (switch_sd_record[3] == 123) {
                message = "SD SECTOR 0 MISMATCH - NO RETRY";
            }
            text(360, message, 104, switch_sd_record[2] == 5 ? SWITCH_GREEN : SWITCH_WHITE);
            hex_line(400, "SD ERROR=", switch_sd_record[3]);
            hex_line(420, "TRANSPORT STAGE=", switch_sd_record[11]);
            hex_line(440, "SDHCI STATUS=", switch_sd_record[12]);
            hex_line(460, "READ WORDS=", switch_sd_record[14]);
            hex_line(480, "REFERENCE CRC32=", switch_sd_record[9]);
            hex_line(500, "READ CRC32=", switch_sd_record[10]);
            hex_line(520, "READINESS FAIL MASK=", switch_sd_record[32]);
            hex_line(540, "PRESENT=", switch_sd_record[18]);
            hex_line(560, "VERSION/POWER/RESET=",
                     (switch_sd_record[23] << 32) | (switch_sd_record[20] << 16) | switch_sd_record[22]);
            hex_line(580, "CLOCK/HOST/BLOCK=",
                     (switch_sd_record[21] << 32) | (switch_sd_record[19] << 16) | switch_sd_record[24]);
            hex_line(600, "MISMATCH BYTES=", switch_sd_record[16]);
            hex_line(620, "FIRST MISMATCH=", switch_sd_record[17]);
            sync_pixels();
            (void)ml_set_interrupts_enabled(previous);
        }
    }
}
#endif
