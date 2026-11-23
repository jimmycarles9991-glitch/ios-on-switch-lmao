constexpr uint32_t kScanEvery = 4;
constexpr uint64_t kScanMaxBytes = 512 << 10, kScanWindowNs = 5000000000ULL;

struct PushCounts {
    uint64_t submits, scanned, bytes, readBytes, unparsed;
    uint64_t passes, targets, linearTargets, draws, wfi, texInval, shaderInval, clears, ceCopies, ceBytes, blits,
        dispatches, acquires, hostFlushes, instructionInval, constantInval;
};

PushCounts gPush;
uint64_t gPushSince;

struct PushParse {
    uint32_t op, subc, mthd, left;  // the method in progress (it may continue in the next push of the submission)
    uint32_t ceLine, ceCount;       // the copy engine's LINE_LENGTH_IN / LINE_COUNT
};

void pushMethod(PushCounts &c, PushParse &p, uint32_t subc, uint32_t mthd, uint32_t data)
{
    if (mthd < 0x100) {                      // host methods, any subchannel
        if (mthd == 0x001c && (data & 0xd))  // SEMAPHORED: ACQUIRE, ACQ_GEQ, ACQ_AND
            c.acquires++;
        else if (mthd == 0x0034 || mthd == 0x0050 || mthd == 0x0078)  // MEM_OP_D, SET_REFERENCE (NVK's membar), WFI
            c.hostFlushes++;
        return;
    }
    const bool target = mthd >= 0x0800 && mthd < 0x0a00;  // SET_COLOR_TARGET_*(j), 64 bytes apart
    switch (subc) {
    case 0:  // MAXWELL_B
        // INVALIDATE_SHADER_CACHES(_NO_WFI) CONSTANT
        c.constantInval += (mthd == 0x021c || mthd == 0x0da4) && (data & (1u << 12));
        if (mthd == 0x0110)
            c.wfi++;  // WAIT_FOR_IDLE
        else if (mthd == 0x1288 || mthd == 0x1338)
            c.texInval++;  // INVALIDATE_TEXTURE_DATA_CACHE(_NO_WFI)
        else if (mthd == 0x021c)
            c.shaderInval++;  // INVALIDATE_SHADER_CACHES (waits for idle)
        else if (mthd == 0x0da4 && (data & 1))
            c.instructionInval++;  // INVALIDATE_SHADER_CACHES_NO_WFI INSTRUCTION
        else if (mthd == 0x1538)
            c.passes++;  // SET_ZT_SELECT: once per vkCmdBeginRendering
        else if (mthd == 0x19d0)
            c.clears++;  // CLEAR_SURFACE
        else if (mthd == 0x1618)
            c.draws++;  // BEGIN outside an MME macro
        else if (target && (mthd & 0x3f) == 0x10 && data)
            c.targets++;  // SET_COLOR_TARGET_FORMAT, not DISABLED
        else if (target && (mthd & 0x3f) == 0x14 && (data & (1u << 12)))
            c.linearTargets++;                                      // SET_COLOR_TARGET_MEMORY LAYOUT_PITCH
        else if (mthd >= 0x3800 && mthd < 0x3c00 && !(mthd & 7)) {  // CALL_MME_MACRO(j)
            const uint32_t j = (mthd - 0x3800) / 8;
            c.draws += j >= 11 && j <= 16;
            c.clears += j == 2;
        }
        break;
    case 1:                              // MAXWELL_COMPUTE_B
        c.dispatches += mthd == 0x02b4;  // SEND_PCAS_A
        c.wfi += mthd == 0x0110;
        break;
    case 3:                         // FERMI_TWOD_A
        c.blits += mthd == 0x08dc;  // PIXELS_FROM_MEMORY_SRC_Y0_INT
        break;
    case 4:  // MAXWELL_DMA_COPY_A
        if (mthd == 0x0418)
            p.ceLine = data;
        else if (mthd == 0x041c)
            p.ceCount = data;
        else if (mthd == 0x0300) {  // LAUNCH_DMA (bit 9 MULTI_LINE)
            c.ceCopies++;
            c.ceBytes += uint64_t(p.ceLine) * ((data & (1u << 9)) ? p.ceCount : 1);
        }
        break;
    default:
        break;
    }
}

void pushParse(PushCounts &c, PushParse &p, const uint32_t *w, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        if (p.left) {  // a data word of the method in progress
            pushMethod(c, p, p.subc, p.mthd, w[i]);
            if (p.op == 1) {  // INC: the next word to the next method
                p.mthd += 4;
            } else if (p.op == 5) {  // ONE_INC: every word after the first to the next method
                p.op = 3;
                p.mthd += 4;
            }
            p.left--;
            continue;
        }
        const uint32_t h = w[i], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                       mthd = (h & 0xfff) << 2;
        if (!h)
            continue;   // padding
        if (op == 4) {  // IMMD_DATA_METHOD: the data is the count field
            pushMethod(c, p, subc, mthd, count);
        } else if (op == 1 || op == 3 || op == 5) {  // INC, NON_INC, ONE_INC
            p = PushParse{op, subc, mthd, count, p.ceLine, p.ceCount};
        } else {
            c.unparsed++;
            return;
        }
    }
}
