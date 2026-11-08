/* Native data-mount proof; only the exact signed PID1 fixture can reach these
 * PC checkpoints. Registry discovery never substitutes a guessed disk name. */
volatile uint64_t switch_block_record[24];
uint64_t switch_block_user_results[40];

static struct {
    uint64_t status;
    char path[2][64];
} block_query;

static uint8_t block_hash_buffer[4096];
static uint64_t block_started, block_frequency;
static unsigned block_enabled, block_finished;

static uint64_t block_ticks(void)
{
    uint64_t t = 0;
    __asm__ volatile("isb\n\tmrs %0,cntpct_el0" : "=r"(t)::"memory");
    return t;
}

static void block_screen(void)
{
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    for (unsigned y = 340; y < 660; y++) {
        for (unsigned x = 16; x < 1264; x++)
            switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
    }
    const char *message = "BUILDING READ ONLY DISK INDEXES";
    if (switch_block_record[3]) {
        message = "NATIVE APFS TEST REFUSED OR FAILED";
    } else if (switch_block_record[2] == 5) {
        message = "APFS FILES VERIFIED - READ ONLY";
    } else if (switch_block_record[2] >= 3) {
        message = "APFS VOLUMES FOUND - READING FILES";
    } else if (switch_block_record[2] == 2) {
        message = "READ ONLY BLOCK DEVICES PUBLISHED";
    }
    text(360, message, 104, switch_block_record[2] == 5 ? SWITCH_GREEN : SWITCH_WHITE);
    hex_line(400, "BLOCK TEST STAGE=", switch_block_record[2]);
    hex_line(420, "BLOCK TEST ERROR=", switch_block_record[3]);
    hex_line(440, "PUBLISH STATUS=", switch_block_record[4]);
    hex_line(460, "VOLUME DISCOVERY STATUS=", switch_block_record[6] | (switch_block_record[7] << 32));
    hex_line(480, "USER STAGE/ERROR=", switch_block_record[8] | (switch_block_record[9] << 32));
    hex_line(500, "OS FILE BYTES=", switch_block_record[10]);
    hex_line(520, "SYSTEMOS FILE BYTES=", switch_block_record[11]);
    hex_line(540, "MATCHING FULL FILE HASHES=", switch_block_record[12] + switch_block_record[13]);
    hex_line(560, "DISCOVERY ATTEMPTS=", switch_block_record[5]);
    hex_line(580, "ELAPSED COUNTER TICKS=", switch_block_record[14]);
    text(620, "OWN SIGNED PID1 - DATA MOUNTS ONLY", 104, SWITCH_WHITE);
    sync_pixels();
    (void)ml_set_interrupts_enabled(previous);
}

/* The block test runs only from the signed PID 1 fixture (switch_diag_user_syscall), which this port never boots. */
static const int block_test = 0;

static void block_start(void)
{
    if (!block_test)
        return;
    block_enabled = 1;
    block_query.status = SWITCH_BLOCK_NOT_READY;
    switch_block_record[0] = UINT64_C(0x53574348424c4f43);
    switch_block_record[1] = 1;
    switch_block_record[2] = 1;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(block_frequency));
    block_started = block_ticks();
    switch_block_record[15] = block_frequency;
    block_screen();
    if (switch_files_record[2] != 9 || switch_files_record[3] || block_frequency < 1000 ||
        block_frequency > 1000000000) {
        switch_block_record[3] = 300;
        block_query.status = SWITCH_BLOCK_NOT_READY;
    } else {
        int status = switch_block_publish();
        switch_block_record[4] = (unsigned)status;
        if (status) {
            switch_block_record[3] = 301;
            block_query.status = (unsigned)status;
        } else {
            switch_block_record[2] = 2;
        }
    }
    switch_block_record[14] = block_ticks() - block_started;
    block_screen();
}

static int block_user_hash(uint64_t address, unsigned size, const uint8_t expected[32])
{
    SHA256_CTX hash;
    SHA256_Init(&hash);
    for (unsigned offset = 0; offset < size;) {
        unsigned n = size - offset;
        if (n > sizeof(block_hash_buffer))
            n = sizeof(block_hash_buffer);
        if (copyin(address + offset, block_hash_buffer, n))
            return 0;
        SHA256_Update(&hash, block_hash_buffer, n);
        offset += n;
    }
    uint8_t got[32];
    SHA256_Final(got, &hash);
    for (unsigned i = 0; i < 32; i++) {
        if (got[i] != expected[i])
            return 0;
    }
    return 1;
}

static int block_volume_result_valid(const uint64_t *r, unsigned size)
{
    static const unsigned flags[] = {1, 3, 5, 8, 10, 12};
    for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        if (r[flags[i]] & (1u << 29))
            return 0;
    }
    return !r[0] && !r[2] && r[4] <= 0x7fffffff && r[6] == size && r[7] && r[7] <= size && !r[9] && !r[11] && !r[13] &&
           !r[14] && !r[15];
}

static void block_checkpoint(uint64_t pc, uint64_t cpsr, int pid, int code, int error, int64_t result)
{
    if (!block_enabled || block_finished || (cpsr & 0x1f) || pid != 1 || code != 20 || error || result != 1 ||
        (pc != SWITCH_USER_BLOCK_QUERY_ACK && pc != SWITCH_USER_BLOCK_FINAL_ACK))
        return;
    switch_block_record[16] = pc;
    switch_block_record[14] = block_ticks() - block_started;
    if (pc == SWITCH_USER_BLOCK_QUERY_ACK) {
        switch_block_record[5]++;
        block_query.status = SWITCH_BLOCK_NOT_READY;
        if (switch_block_record[3]) {
            block_query.status = SWITCH_BLOCK_CORRUPT;
        } else if (switch_block_record[5] > 200 || switch_block_record[14] > block_frequency * 120) {
            switch_block_record[3] = 302;
            block_query.status = SWITCH_BLOCK_TIMEOUT;
        } else {
            int a = switch_block_volume_path(0, block_query.path[0], 64);
            int b = switch_block_volume_path(1, block_query.path[1], 64);
            switch_block_record[6] = (unsigned)a;
            switch_block_record[7] = (unsigned)b;
            if (a > SWITCH_BLOCK_NOT_READY || b > SWITCH_BLOCK_NOT_READY) {
                switch_block_record[3] = 303;
                block_query.status = SWITCH_BLOCK_CORRUPT;
            } else if (!a && !b) {
                block_query.status = SWITCH_BLOCK_OK;
                switch_block_record[2] = 3;
            }
        }
        if (copyout(&block_query, SWITCH_USER_BLOCK_QUERY, sizeof(block_query)))
            switch_block_record[3] = 304;
        if (switch_block_record[5] == 1 || switch_block_record[2] == 3 || switch_block_record[3])
            block_screen();
        return;
    }
    block_finished = 1;
    if (copyin(SWITCH_USER_BLOCK_RESULTS, switch_block_user_results, sizeof(switch_block_user_results))) {
        switch_block_record[3] = 305;
        block_screen();
        return;
    }
    const uint64_t *r = switch_block_user_results;
    switch_block_record[8] = r[0];
    switch_block_record[9] = r[1];
    switch_block_record[10] = r[14];
    switch_block_record[11] = r[30];
    if (switch_block_record[3] || switch_block_record[2] != 3 || r[0] != 4 || r[1] || r[4] != switch_block_record[5] ||
        r[4] < 1 || r[4] > 200 || r[5] + 1 != r[4] || r[6] || r[7] ||
        !block_volume_result_valid(r + 8, switch_block_file_size_0) ||
        !block_volume_result_valid(r + 24, switch_block_file_size_1)) {
        if (!switch_block_record[3])
            switch_block_record[3] = 306;
        block_screen();
        return;
    }
    switch_block_record[2] = 4;
    switch_block_record[12] =
        block_user_hash(SWITCH_USER_BLOCK_OS_BYTES, switch_block_file_size_0, switch_block_file_digest_0);
    switch_block_record[13] =
        block_user_hash(SWITCH_USER_BLOCK_SYSTEM_BYTES, switch_block_file_size_1, switch_block_file_digest_1);
    if (switch_block_record[12] && switch_block_record[13]) {
        switch_block_record[2] = 5;
    } else {
        switch_block_record[3] = 307;
    }
    block_screen();
}
