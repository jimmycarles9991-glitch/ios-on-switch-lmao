// SPDX-License-Identifier: MIT
// nvdec (r7-raw): see nvdec.h. Method numbers: NVIDIA's class headers as Mesa ships them, through kits/SwitchGM20B (MIT).
// Macros run through Mesa's Fermi MME simulator (src/nouveau/mme/mme_fermi_sim.c, MIT; third_party/mesa/libnouveau_mme.a).
#include "nvdec.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "TegraGM20BLayout.h"
#ifdef NVDEC_TWOD
#include "../fx/test/twod_sim.h"
#endif

#include <algorithm>
#include <cstring>

extern "C" {
struct mme_fermi_inst;

struct mme_sim_state_ops {
    uint32_t (*load)(void *handler);
    uint32_t (*state)(void *handler, uint16_t addr);
    void (*mthd)(void *handler, uint16_t addr, uint32_t data);
    void (*barrier)(void *handler);
    uint32_t *(*map_dram)(void *handler, uint32_t idx);
};

void mme_fermi_decode(struct mme_fermi_inst *insts, const uint32_t *in, uint32_t inst_count);
void mme_fermi_sim_core(uint32_t inst_count, const struct mme_fermi_inst *insts,
                        const struct mme_sim_state_ops *state_ops, void *state_handler);
}

namespace nvdec {
namespace {
// 3D class (MAXWELL_B) methods used here.
enum : uint32_t {
    WAIT_FOR_IDLE = NVB197_WAIT_FOR_IDLE,
    LOAD_MME_INSTRUCTION_RAM_POINTER = NVB197_LOAD_MME_INSTRUCTION_RAM_POINTER,
    LOAD_MME_INSTRUCTION_RAM = NVB197_LOAD_MME_INSTRUCTION_RAM,
    LOAD_MME_START_ADDRESS_RAM_POINTER = NVB197_LOAD_MME_START_ADDRESS_RAM_POINTER,
    LOAD_MME_START_ADDRESS_RAM = NVB197_LOAD_MME_START_ADDRESS_RAM,
    INVALIDATE_SHADER_CACHES = NVB197_INVALIDATE_SHADER_CACHES,
    COLOR_TARGET = NVB197_SET_COLOR_TARGET_A(0),
    VIEWPORT = NVB197_SET_VIEWPORT_SCALE_X(0),
    CLEAR_RECT_H = NVB197_SET_CLEAR_RECT_HORIZONTAL,
    CLEAR_RECT_V = NVB197_SET_CLEAR_RECT_VERTICAL,
    VERTEX_ARRAY_START = NVB197_SET_VERTEX_ARRAY_START,
    DRAW_VERTEX_ARRAY = NVB197_DRAW_VERTEX_ARRAY,
    COLOR_CLEAR_VALUE = NVB197_SET_COLOR_CLEAR_VALUE(0),
    Z_CLEAR_VALUE = NVB197_SET_Z_CLEAR_VALUE,
    STENCIL_CLEAR_VALUE = NVB197_SET_STENCIL_CLEAR_VALUE,
    INVALIDATE_SHADER_CACHES_NO_WFI = NVB197_INVALIDATE_SHADER_CACHES_NO_WFI,
    PIXEL_SHADER_BARRIER = NVB197_PIXEL_SHADER_BARRIER,
    NO_OPERATION = NVB197_NO_OPERATION,
    INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED = SWITCH_NVB197_INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED,
    TILED_CACHE_BARRIER = SWITCH_NVB197_TILED_CACHE_BARRIER,
    TILED_CACHE_FLUSH = SWITCH_NVB197_TILED_CACHE_FLUSH,
    ZT_A = NVB197_SET_ZT_A,
    ZT_B = NVB197_SET_ZT_B,
    CLEAR_SURFACE_CONTROL = NVB197_SET_CLEAR_SURFACE_CONTROL,
    FLUSH_PENDING_WRITES = NVB197_FLUSH_PENDING_WRITES,
    CT_SELECT = NVB197_SET_CT_SELECT,
    INVALIDATE_TEXTURE_DATA_CACHE = NVB197_INVALIDATE_TEXTURE_DATA_CACHE,
    INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI = NVB197_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI,
    INVALIDATE_SAMPLER_CACHE_NO_WFI = NVB197_INVALIDATE_SAMPLER_CACHE_NO_WFI,
    INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI = NVB197_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI,
    ZT_SELECT = NVB197_SET_ZT_SELECT,
    TSC_POOL_A = NVB197_SET_TEX_SAMPLER_POOL_A,
    TSC_POOL_B = NVB197_SET_TEX_SAMPLER_POOL_B,
    TSC_POOL_C = NVB197_SET_TEX_SAMPLER_POOL_C,
    TIC_POOL_A = NVB197_SET_TEX_HEADER_POOL_A,
    TIC_POOL_B = NVB197_SET_TEX_HEADER_POOL_B,
    TIC_POOL_C = NVB197_SET_TEX_HEADER_POOL_C,
    PROGRAM_REGION_A = NVB197_SET_PROGRAM_REGION_A,
    PROGRAM_REGION_B = NVB197_SET_PROGRAM_REGION_B,
    END = NVB197_END,
    BEGIN = NVB197_BEGIN,
    RENDER_ENABLE_OVERRIDE = NVB197_SET_RENDER_ENABLE_OVERRIDE,
    CLEAR_SURFACE = NVB197_CLEAR_SURFACE,
    COLOR_COMPRESSION = NVB197_SET_COLOR_COMPRESSION(0),
    Z_COMPRESSION = NVB197_SET_Z_COMPRESSION,
    ZT_SIZE_A = NVB197_SET_ZT_SIZE_A,
    REPORT_SEMAPHORE_A = NVB197_SET_REPORT_SEMAPHORE_A,
    REPORT_SEMAPHORE_D = NVB197_SET_REPORT_SEMAPHORE_D,
    VERTEX_STREAM_A = NVB197_SET_VERTEX_STREAM_A_FORMAT(0),
    VERTEX_STREAM_LIMIT = NVB197_SET_VERTEX_STREAM_LIMIT_A_A(0),
    PIPELINE = NVB197_SET_PIPELINE_SHADER(0),
    CB_SELECTOR_A = NVB197_SET_CONSTANT_BUFFER_SELECTOR_A,
    CB_SELECTOR_B = NVB197_SET_CONSTANT_BUFFER_SELECTOR_B,
    CB_SELECTOR_C = NVB197_SET_CONSTANT_BUFFER_SELECTOR_C,
    SET_FALCON04 = NVB197_SET_FALCON04,
    LOAD_CB_OFFSET = NVB197_LOAD_CONSTANT_BUFFER_OFFSET,
    LOAD_CB = NVB197_LOAD_CONSTANT_BUFFER(0),
    BIND_GROUP_CB = NVB197_BIND_GROUP_CONSTANT_BUFFER(0),
    MME_SCRATCH = NVB197_SET_MME_SHADOW_SCRATCH(0),
    CALL_MME = NVB197_CALL_MME_MACRO(0),
};

struct Sim {
    Decoder *d;
    const std::vector<uint32_t> *params;
    size_t at;
    std::function<void(uint16_t, uint32_t)> emit;
    std::function<uint32_t(uint16_t)> state;
};

uint32_t simLoad(void *h)
{
    Sim *s = static_cast<Sim *>(h);
    return s->at < s->params->size() ? (*s->params)[s->at++] : 0;
}

uint32_t simState(void *h, uint16_t addr)
{
    return static_cast<Sim *>(h)->state(addr);
}

void simMthd(void *h, uint16_t addr, uint32_t data)
{
    static_cast<Sim *>(h)->emit(addr, data);
}

void simBarrier(void *)
{}

uint32_t *simDram(void *, uint32_t)
{
    return nullptr;
}

// a record's text without the run's resource names (R<n>): what goes into a hash (names are compared as text, by tools/eqcheck.py
// in order of first appearance, so the two runs' numbering may differ)
std::string nameless(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == 'R' && i + 1 < s.size() && isdigit(s[i + 1]) && (i == 0 || !isalnum(s[i - 1]))) {
            while (i + 1 < s.size() && isdigit(s[i + 1]))
                i++;
            out += 'R';
            continue;
        }
        out += s[i];
    }
    return out;
}

// An indexed draw of at most this many indices prints them by value (eqcheck.py joins a split draw's runs from them)
constexpr uint32_t kListedIndices = 256;

std::string hex(uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "%llx", (unsigned long long)v);
    return b;
}

// Registers that are not state a draw reads, or that only mean something as an address, or that NVK uses as its own
// bookkeeping: never compared as plain values.
bool ignored3d(uint32_t m)
{
    if (m < 0x0200)
        return true;  // control, MME RAM loads, WFI
    if (m == INVALIDATE_SHADER_CACHES || m == INVALIDATE_SHADER_CACHES_NO_WFI || m == INVALIDATE_TEXTURE_DATA_CACHE ||
        m == INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI || m == INVALIDATE_SAMPLER_CACHE_NO_WFI ||
        m == INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI || m == FLUSH_PENDING_WRITES || m == TILED_CACHE_BARRIER ||
        m == TILED_CACHE_FLUSH)
        return true;
    if (m >= CLEAR_RECT_H && m <= COLOR_CLEAR_VALUE + 0xc)
        return true;  // clear rect/vertex array triggers/clear values
    if (m == CLEAR_SURFACE_CONTROL || m == CLEAR_SURFACE || m == RENDER_ENABLE_OVERRIDE)
        return true;
    if (m == BEGIN || m == END)
        return true;
    if (m >= REPORT_SEMAPHORE_A && m <= REPORT_SEMAPHORE_D)
        return true;
    if (m >= CB_SELECTOR_A && m < 0x2600)
        return true;  // cbuf selector/loads/binds (compared as cbufs)
    if (m >= MME_SCRATCH)
        return true;  // MME shadow scratch, macro calls
    // addresses (printed by name or by content instead)
    if (m >= COLOR_TARGET && m < COLOR_TARGET + 0x200 && ((m & 0x3f) == 0 || (m & 0x3f) == 4))
        return true;
    if (m == ZT_A || m == ZT_B || m == PROGRAM_REGION_A || m == PROGRAM_REGION_B)
        return true;
    if (m == TIC_POOL_A || m == TIC_POOL_B || m == TSC_POOL_A || m == TSC_POOL_B)
        return true;
    if (m >= VERTEX_STREAM_A && m < VERTEX_STREAM_A + 0x200 && ((m & 0xf) == 4 || (m & 0xf) == 8))
        return true;
    if (m >= VERTEX_STREAM_LIMIT && m < VERTEX_STREAM_LIMIT + 0x100)
        return true;
    if (m >= PIPELINE && m < PIPELINE + 0x180 && (m & 0x3f) == 4)
        return true;  // program offsets: compared as programs
    // other address-bearing state NVK sets once (shader local memory, zcull, query/ICB, index buffer, MME DMA), and an
    // indexed draw's first index and count (SET_INDEX_BUFFER_F, DRAW_INDEX_BUFFER: its ib line's)
    static const uint32_t addrs[] = {0x0790, 0x0794, 0x07e0, 0x07e4, 0x07e8, 0x07ec, 0x07f0,
                                     0x07f4, 0x07f8, 0x07fc, 0x0f84, 0x0f88, 0x1550, 0x1554,
                                     0x17c8, 0x17cc, 0x17d0, 0x17d4, 0x17d8, 0x17dc, 0x17e0};
    return std::find(std::begin(addrs), std::end(addrs), m) != std::end(addrs);
}
}  // namespace

uint64_t Decoder::fnv(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

Decoder::Decoder(MapFn map, FILE *out) : map_(std::move(map)), out_(out)
{}
#ifdef NVDEC_TWOD
void Decoder::twod(uint32_t subc, uint32_t mthd, uint32_t data)
{
    auto *sim = static_cast<twodsim::Sim *>(twod_);
    if (!sim) {
        twod_ = sim = new twodsim::Sim;
        // NVDEC_TWOD_QUIRK=gm20b: the engine as the device measured it (its centre origin half a texel off, fx::kGm20bSampling)
        if (const char *q = getenv("NVDEC_TWOD_QUIRK"))
            sim->quirkCentreHalfTexel = !strcmp(q, "gm20b");
        sim->host = [this](uint64_t va, uint64_t bytes) {
            return const_cast<uint8_t *>(map_ ? map_(va, bytes) : nullptr);
        };
    }
    if (!sim->feed(subc, mthd, data)) {
        fprintf(stderr, "nvdec: 2D engine: %s\n", sim->error().c_str());
        if (out_)
            fprintf(out_, "TWOD_ERROR %s\n", sim->error().c_str());
        delete sim;
        twod_ = nullptr;
    }
}
#else
void Decoder::twod(uint32_t, uint32_t, uint32_t)
{}
#endif

void Decoder::submitBegin()
{
    submits++;
    op_ = subc_ = mthd_ = left_ = 0;
    writtenThisSubmit_.clear();
}

void Decoder::submitEnd()
{
    if (pendingMacro_ >= 0)
        runMacro();
    landCopies();
    static const uint64_t trace = getenv("NVDEC_TRACE") ? strtoull(getenv("NVDEC_TRACE"), nullptr, 0) : 0;
    if (submits == trace) {
        for (int sc = 0; sc < 2; sc++) {
            std::string line = "S regs " + std::to_string(sc);
            for (uint32_t i = 0; i < 0x4000; i++)
                if (reg_[sc][i]) {
                    char b[24];
                    snprintf(b, sizeof b, " %x=%x", i * 4, reg_[sc][i]);
                    line += b;
                }
            fprintf(out_, "%s\n", line.c_str());
        }
    }
    if (!pendingBarriers_.empty()) {
        std::string s;
        for (auto &b : pendingBarriers_)
            s += (s.empty() ? "" : ",") + b;
        fprintf(out_, "END-OF-SUBMIT barriers=%s\n", s.c_str());
        pendingBarriers_.clear();
    }
    if (compSubmit.any())
        fprintf(out_, "COMP draws=%llu clears=%llu zbc=%llu zbcKiB=%llu\n", (unsigned long long)compSubmit.draws,
                (unsigned long long)compSubmit.clears, (unsigned long long)compSubmit.zbc,
                (unsigned long long)(compSubmit.zbcBytes >> 10));
    compSubmit = {};
    fprintf(out_, "SUBMIT %llu\n", (unsigned long long)submits);
}

void Decoder::push(const uint32_t *w, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        if (left_) {
            method(subc_, mthd_, w[i], false);
            if (op_ == 1)
                mthd_ += 4;
            else if (op_ == 5) {
                op_ = 3;
                mthd_ += 4;
            }
            left_--;
            continue;
        }
        const uint32_t h = w[i];
        if (!h)
            continue;
        const uint32_t op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7, mthd = (h & 0xfff) << 2;
        if (op == 4)
            method(subc, mthd, count, false);
        else if (op == 1 || op == 3 || op == 5) {
            op_ = op;
            subc_ = subc;
            mthd_ = mthd;
            left_ = count;
        } else {
            badHeaders++;
            return;
        }
    }
}

void Decoder::barrier(const std::string &what)
{
    pendingBarriers_.insert(what);
}

void Decoder::method(uint32_t subc, uint32_t mthd, uint32_t data, bool fromMme)
{
    if (subc > 4)
        return;
    // NVDEC_TRACE=N: every method the stream itself writes in submissions 1..N (the context init NVK records), for tools/mkpack.py
    static const uint64_t trace = getenv("NVDEC_TRACE") ? strtoull(getenv("NVDEC_TRACE"), nullptr, 0) : 0;
    if (!fromMme && submits <= trace)
        fprintf(out_, "T %u %x %x\n", subc, mthd, data);
    if (!fromMme && pendingMacro_ >= 0 && subc == 0 && mthd == CALL_MME + 8 * uint32_t(pendingMacro_) + 4) {
        params_.push_back(data);
        return;
    }
    if (!fromMme && pendingMacro_ >= 0)
        runMacro();
    if (subc != 4 || mthd < 0x100)
        landCopies();
    if (mthd < 0x100) {  // host class methods on any subchannel
        reg_[subc][mthd >> 2] = data;
        if (mthd == 0x78)
            barrier("HOST_WFI");
        else if (mthd == 0x1c && (data & 0xd))
            barrier("SEM_ACQ");
        else if (mthd == 0x34)
            barrier("MEM_OP");
        return;
    }
    if (subc != 0) {
        reg_[subc][mthd >> 2] = data;
        if (subc == 1)
            computeMethod(mthd, data);
        if (subc == 4 && mthd == 0x0300) {
            barrier("CE_COPY");
            copyEngine(data);
        }
        if (subc == 3 && mthd == 0x08dc)
            fprintf(out_, "BLIT2D\n");
#ifdef NVDEC_TWOD
        if (subc == 3)
            twod(subc, mthd, data);
#endif
        return;
    }
#ifdef NVDEC_TWOD
    if (mthd == 0x0110 || mthd == 0x1288)
        twod(subc, mthd, data);
#endif
    // 3D
    if (!fromMme && mthd >= CALL_MME && mthd < CALL_MME + 0x800) {
        const uint32_t j = (mthd - CALL_MME) / 8;
        pendingMacro_ = int(j);
        params_.assign(1, data);
        return;
    }
    switch (mthd) {
    case LOAD_MME_INSTRUCTION_RAM_POINTER:
        mmeRamPtr_ = data;
        break;
    case LOAD_MME_INSTRUCTION_RAM:
        if (mmeRamPtr_ < mmeRam_.size())
            mmeRam_[mmeRamPtr_++] = data;
        break;
    case LOAD_MME_START_ADDRESS_RAM_POINTER:
        mmeStartPtr_ = data;
        break;
    case LOAD_MME_START_ADDRESS_RAM:
        if (mmeStartPtr_ < mmeStart_.size())
            mmeStart_[mmeStartPtr_++] = data;
        break;
    case WAIT_FOR_IDLE:
        barrier("WFI");
        break;
    case PIXEL_SHADER_BARRIER:
        barrier("PSB");
        break;
    case TILED_CACHE_BARRIER:
        barrier("TCBAR");
        break;
    case TILED_CACHE_FLUSH:
        barrier("TCFLUSH");
        break;
    case NO_OPERATION:
        // raw_gpu.cpp's marks (SWITCH_RAW_HARNESS_MARKS builds): a fetch barrier left out, a draw's run after its first
        if (data == 0x0e11de)
            barrier("ELIDED");
        else if (data == 0x5b1175)
            barrier("SPLIT");
        break;
    case INVALIDATE_TEXTURE_DATA_CACHE:
    case INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI:
    case INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED:
        barrier("TEXINV");
        break;
    case INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI:
        barrier("TICINV");
        break;
    case INVALIDATE_SAMPLER_CACHE_NO_WFI:
        barrier("TSCINV");
        break;
    case INVALIDATE_SHADER_CACHES:
    case INVALIDATE_SHADER_CACHES_NO_WFI:
        barrier("SHINV");
        if (data & 1)  // INSTRUCTION
            barrier("ICINV");
        if (data & 0x1000) {  // CONSTANT
            barrier("CBINV");
            constantStale_.clear();
        }
        break;
    default:
        break;
    }
    if (mthd >= LOAD_CB && mthd < LOAD_CB + 0x40) {
        const uint64_t base = addr3(CB_SELECTOR_B, CB_SELECTOR_C);
        const uint32_t off = r3(LOAD_CB_OFFSET);
        cbufWrites_[base + off] = data;
        writtenThisSubmit_.insert(base + off);
        reg_[0][LOAD_CB_OFFSET >> 2] = off + 4;
        return;
    }
    if (mthd >= BIND_GROUP_CB && mthd < BIND_GROUP_CB + 0xa0 && ((mthd - BIND_GROUP_CB) & 0x1f) == 0) {
        const uint32_t g = (mthd - BIND_GROUP_CB) / 0x20, slot = (data >> 4) & 0x1f;
        if (g < 5 && slot < 18) {
            Bind &b = binds_[g][slot];
            b.valid = data & 1;
            b.addr = addr3(CB_SELECTOR_B, CB_SELECTOR_C);
            b.size = r3(CB_SELECTOR_A);
        }
        return;
    }
    if (mthd == DRAW_VERTEX_ARRAY) {
        reg_[0][mthd >> 2] = data;
        draw("DRAW", r3(VERTEX_ARRAY_START), data);
        return;
    }
    // r8-rawca: an indexed draw (SET_INDEX_BUFFER_F's first index, DRAW_INDEX_BUFFER's count; NVK's NVK_MME_DRAW_INDEXED emits
    // them): the vertices it reads are those up to its largest index; its indices are printed by content
    if (mthd == 0x17e0) {
        reg_[0][mthd >> 2] = data;
        const uint32_t size = r3(0x17d8) == 2 ? 4 : r3(0x17d8) == 1 ? 2 : 1, first = r3(0x17dc);
        const uint64_t ib = addr3(0x17c8, 0x17cc) + uint64_t(first) * size;
        std::vector<uint8_t> idx(size_t(data) * size);
        read(ib, idx.data(), idx.size());
        uint32_t most = 0;
        for (uint32_t i = 0; i < data; i++) {
            uint32_t v = 0;
            memcpy(&v, &idx[size_t(i) * size], size);
            most = std::max(most, v);
        }
        indexed_ = true;
        draw("DRAW", 0, most + 1);
        std::string indices = contentHash(ib, idx.size(), 0, false, false);
        if (data <= kListedIndices) {
            indices = "";
            for (uint32_t i = 0; i < data; i++) {
                uint32_t v = 0;
                memcpy(&v, &idx[size_t(i) * size], size);
                indices += (i ? "," : "") + hex(v);
            }
        }
        fprintf(out_, "  ib size=%u first=%u count=%u %s=%s restart=%x end=%llx\n", size, first, data,
                data <= kListedIndices ? "idx" : "data", indices.c_str(), r3(0x1648),
                (unsigned long long)(addr3(0x17d0, 0x17d4) - addr3(0x17c8, 0x17cc)));
        indexed_ = false;
        return;
    }
    if (mthd == CLEAR_SURFACE) {
        clear(data);
        return;
    }
    if (mthd == SET_FALCON04) {
        // FECS firmware writes a privileged register for NVK (nvk_mme_set_priv_reg: value and mask in MME scratch 1, 2) and
        // acknowledges in scratch 0, which the macro waits for: done at once here.
        char b[64];
        snprintf(b, sizeof b, "PRIV:%x=%x/%x", data, reg_[0][(MME_SCRATCH >> 2) + 1], reg_[0][(MME_SCRATCH >> 2) + 2]);
        barrier(b);
        reg_[0][MME_SCRATCH >> 2] = 1;
        return;
    }
    reg_[0][mthd >> 2] = data;
}

void Decoder::landCopies()
{
    for (const CopyStore &s : copyStores_)
        for (size_t k = 0; k < s.bytes.size();) {
            size_t end = k + 1;
            if (s.written.empty())
                end = s.bytes.size();
            else
                while (end < s.bytes.size() && s.written[end] == s.written[k])
                    end++;
            if (s.written.empty() || s.written[k])
                storeCopy(s.at + k, &s.bytes[k], end - k);
            k = end;
        }
    copyStores_.clear();
}

// NVDEC_WRITETHROUGH: the copy lands in the memory behind the destination too, as the GPU's would, so a host test reads its
// result back on the CPU (pack/cap.cpp maps every BO) (and not in the overlay: the CPU may rewrite that memory later, which the
// overlay would hide from the next read)
void Decoder::storeCopy(uint64_t at, const uint8_t *bytes, uint64_t n)
{
    static const bool through = getenv("NVDEC_WRITETHROUGH") != nullptr;
    if (through && map_)
        if (const uint8_t *h = map_(at, n)) {
            memcpy(const_cast<uint8_t *>(h), bytes, n);
            return;
        }
    for (uint64_t k = 0; k < n; k += 4) {
        uint32_t w = 0;
        if (k + 4 <= n && ((at + k) & 3) == 0)
            memcpy(&w, &bytes[k], 4);
        else {  // an unaligned edge: merge into the word
            const uint64_t a = (at + k) & ~3ull;
            read(a, &w, 4);
            for (uint64_t b = 0; b < 4 && k + b < n; b++)
                reinterpret_cast<uint8_t *>(&w)[((at + k) & 3) + b < 4 ? ((at + k) & 3) + b : 3] = bytes[k + b];
            cbufWrites_[a] = w;
            continue;
        }
        cbufWrites_[at + k] = w;
    }
}

// MAXWELL_DMA_COPY_A LAUNCH_DMA: a copy (NVK's upload queue puts shaders and data in place with these; the raw path's
// copies) or, with REMAP to constant A, a fill. Either side may be block-linear (SET_SRC/DST_BLOCK_SIZE: one GOB wide, its
// block height, the surface's width in elements and height, the copy's origin), addressed as TegraGM20BLayout.h lays the
// GPU's tiled memory out; NVK's optimal images and the raw path's block-linear textures are. The GPU's writes land in the
// same overlay as LOAD_CONSTANT_BUFFER's.
void Decoder::copyEngine(uint32_t launch)
{
    const uint32_t *r = reg_[4];
    const uint64_t src = (uint64_t(r[0x400 / 4] & 0xff) << 32) | r[0x404 / 4],
                   dst = (uint64_t(r[0x408 / 4] & 0xff) << 32) | r[0x40c / 4];
    const uint32_t pitchIn = r[0x410 / 4], pitchOut = r[0x414 / 4], length = r[0x418 / 4];
    const uint32_t lines = (launch & (1u << 9)) ? r[0x41c / 4] : 1;
    const bool srcPitch = launch & (1u << 7), dstPitch = launch & (1u << 8);
    // remap: each destination component of COMPONENT_SIZE bytes from a source component (SRC_X..W of the source element of
    // NUM_SRC_COMPONENTS), constant A or B, or left as it was (NO_WRITE); the line length counts elements (nouveau_copy_rect).
    // NVK's two uses: DST_X CONST_A, the fill (vkCmdFillBuffer), and DST_X..W = SRC_X..W, an image copy; the raw path's planar
    // conversion (r8-rawca: a 4:4:4 two-plane surface's planes into one RGBA8 image) leaves components with NO_WRITE.
    uint32_t elem = 1, srcElem = 1, comp = 0, csize = 1, nsrc = 1, ndst = 1;
    const uint32_t consts[2] = {r[0x700 / 4], r[0x704 / 4]};
    bool remap = false, plain = true;
    if (launch & (1u << 10)) {
        remap = true;
        comp = r[0x708 / 4];
        csize = ((comp >> 16) & 3) + 1;
        nsrc = ((comp >> 20) & 3) + 1;
        ndst = ((comp >> 24) & 3) + 1;
        elem = csize * ndst;
        srcElem = csize * nsrc;
        for (uint32_t c = 0; c < ndst; c++)
            plain = plain && ((comp >> (4 * c)) & 7) == c && c < nsrc;
        plain = plain && nsrc == ndst;
    }
    bool reads = !remap || plain;
    for (uint32_t c = 0; remap && c < ndst; c++)
        reads = reads || ((comp >> (4 * c)) & 7) < 4;

    // where element e of line y lies: rows `pitch` apart from `base`, or a block-linear surface (its six block words at `at`:
    // block height and depth, width in elements, height, depth and the layer the copy reads or writes)
    struct Side {
        bool tiled;
        uint64_t base;
        uint32_t pitch, yLog2, rowBytes, ox, oy, zLog2 = 0, height = 0, layer = 0;
    };

    auto side = [&](bool isPitch, uint64_t base, uint32_t pitch, uint32_t at, uint32_t elemBytes) {
        Side d{!isPitch, base, pitch, 0, 0, 0, 0};
        if (d.tiled) {
            d.yLog2 = (r[at / 4] >> 4) & 0xf;
            d.zLog2 = (r[at / 4] >> 8) & 0xf;
            d.rowBytes = r[at / 4 + 1] * elemBytes;
            d.height = r[at / 4 + 2];
            d.layer = r[at / 4 + 4];
            d.ox = r[at / 4 + 5] & 0xffff;
            d.oy = r[at / 4 + 5] >> 16;
        }
        return d;
    };
    const Side in = side(srcPitch, src, pitchIn, 0x728, srcElem), out = side(dstPitch, dst, pitchOut, 0x70c, elem);
    // a constant buffer is pitch memory: block-linear destinations are images
    if (dstPitch && length && lines) {
        constantStale_.push_back({dst, dst + uint64_t(lines - 1) * pitchOut + uint64_t(length) * elem});
        // one hull of them all: may see a stale read where there is none, never miss one
        if (constantStale_.size() > 1024) {
            uint64_t lo = ~0ull, hi = 0;
            for (const auto &r : constantStale_) {
                lo = std::min(lo, r.first);
                hi = std::max(hi, r.second);
            }
            constantStale_.assign(1, {lo, hi});
        }
    }
    auto addr = [](const Side &d, uint32_t e, uint32_t y, uint32_t elemBytes) {
        if (!d.tiled)
            return d.base + uint64_t(y) * d.pitch + uint64_t(e) * elemBytes;
        if (!d.zLog2 && !d.layer)
            return d.base + tgm_layout_offset_of(d.rowBytes, uint8_t(d.yLog2), (d.ox + e) * elemBytes, d.oy + y);
        // a plane of a 3D surface: blocks along a row, down the plane, then through the depth; GOBs down a block, then
        // through its planes
        const uint32_t xB = (d.ox + e) * elemBytes, row = d.oy + y, blockRows = 8u << d.yLog2,
                       blockPlanes = 1u << d.zLog2;
        const uint64_t blocksDown = (d.height + blockRows - 1) / blockRows;
        const uint64_t block =
            (uint64_t(d.layer / blockPlanes) * blocksDown + row / blockRows) * (d.rowBytes / 64) + xB / 64;
        const uint64_t gob = uint64_t(d.layer % blockPlanes) * (blockRows / 8) + (row % blockRows) / 8;
        return d.base + block * (512ull << (d.yLog2 + d.zLog2)) + gob * 512 + tgm_layout_gob_offset(xB, row);
    };
    if ((launch & 3) != 1)  // not PIPELINED
        landCopies();
    // NO_WRITE and missing source components leave the destination's bytes as they are when the copy lands
    std::vector<uint8_t> written;
    for (uint32_t c = 0; remap && !plain && c < ndst; c++) {
        const uint32_t sel = (comp >> (4 * c)) & 7;
        written.insert(written.end(), csize, (sel < 4 && sel < nsrc) || sel == 4 || sel == 5);
    }
    const size_t first = copyStores_.size();
    auto store = [&](uint64_t at, const uint8_t *bytes, uint64_t n) {
        if (copyStores_.size() == first || copyStores_.back().at + copyStores_.back().bytes.size() != at)
            copyStores_.push_back({at, {}, {}});
        CopyStore &s = copyStores_.back();
        s.bytes.insert(s.bytes.end(), bytes, bytes + n);
        for (uint64_t k = 0; !written.empty() && k < n; k += elem)
            s.written.insert(s.written.end(), written.begin(), written.end());
    };
    const uint64_t bytes = uint64_t(length) * elem;
    std::vector<uint8_t> line(bytes), from(uint64_t(length) * srcElem);
    for (uint32_t y = 0; y < lines; y++) {
        if (reads) {
            if (in.tiled)
                for (uint32_t e = 0; e < length; e++)
                    read(addr(in, e, y, srcElem), &from[uint64_t(e) * srcElem], srcElem);
            else
                read(addr(in, 0, y, srcElem), from.data(), from.size());
        }
        if (remap && !plain) {
            for (uint32_t e = 0; e < length; e++)
                for (uint32_t c = 0; c < ndst; c++) {
                    const uint32_t sel = (comp >> (4 * c)) & 7;
                    uint8_t *o = &line[uint64_t(e) * elem + c * csize];
                    if (sel < 4 && sel < nsrc)
                        memcpy(o, &from[uint64_t(e) * srcElem + sel * csize], csize);
                    else if (sel == 4 || sel == 5)
                        for (uint32_t k = 0; k < csize; k++)
                            o[k] = uint8_t(consts[sel - 4] >> (8 * k));
                }
        } else {
            line.assign(from.begin(), from.begin() + bytes);
        }
        if (out.tiled)
            for (uint32_t e = 0; e < length; e++)
                store(addr(out, e, y, elem), &line[uint64_t(e) * elem], elem);
        else
            store(addr(out, 0, y, elem), line.data(), bytes);
    }
}

void Decoder::runMacro()
{
    const int j = pendingMacro_;
    pendingMacro_ = -1;
    macroCalls++;
    const uint32_t start = j < int(mmeStart_.size()) ? mmeStart_[j] : 0;
    if (start >= mmeRam_.size()) {
        unknownMacros++;
        return;
    }
    const uint32_t count = uint32_t(mmeRam_.size()) - start;
    std::vector<uint8_t> insts(size_t(count) * 64);  // sizeof(struct mme_fermi_inst) is well under 64 bytes
    mme_fermi_decode(reinterpret_cast<mme_fermi_inst *>(insts.data()), mmeRam_.data() + start, count);
    std::vector<uint32_t> params = params_;
    Sim sim{this, &params, 0,
            [this](uint16_t a, uint32_t d) {
                method(0, a, d, true);
            },
            [this](uint16_t a) {
                return reg_[0][(a >> 2) & 0x3fff];
            }};
    static const mme_sim_state_ops ops = {simLoad, simState, simMthd, simBarrier, simDram};
    mme_fermi_sim_core(count, reinterpret_cast<mme_fermi_inst *>(insts.data()), &ops, &sim);
}

bool Decoder::read(uint64_t va, void *dst, uint64_t bytes)
{
    const uint8_t *src = map_ ? map_(va, bytes) : nullptr;
    if (src)
        memcpy(dst, src, bytes);
    else if (map_ && bytes) {  // across binds (NVK grows its shader heap by binding more BOs after it): page by page
        bool any = false;
        for (uint64_t at = 0; at < bytes;) {
            const uint64_t n = std::min<uint64_t>(bytes - at, 0x1000 - ((va + at) & 0xfff));
            const uint8_t *s = map_(va + at, n);
            if (s) {
                memcpy(static_cast<uint8_t *>(dst) + at, s, n);
                any = true;
            } else
                memset(static_cast<uint8_t *>(dst) + at, 0, n);
            at += n;
        }
        src = any ? static_cast<const uint8_t *>(dst) : nullptr;
    } else
        memset(dst, 0, bytes);
    // the GPU's writes over it: every overlay word the range overlaps, its overlapping bytes (a row of a copy whose length is no
    // multiple of 4 ends in part of a word: r18, NVDEC_TEXELS over rows of 1025 bytes)
    for (auto it = cbufWrites_.lower_bound(va >= 3 ? va - 3 : 0); it != cbufWrites_.end() && it->first < va + bytes;
         ++it) {
        const uint64_t lo = std::max(it->first, va), hi = std::min(it->first + 4, va + bytes);
        if (lo < hi)
            memcpy(static_cast<uint8_t *>(dst) + (lo - va),
                   reinterpret_cast<const uint8_t *>(&it->second) + (lo - it->first), hi - lo);
    }
    return src != nullptr;
}

std::map<uint64_t, int> &Decoder::names()
{
    static std::map<uint64_t, int> n;
    return n;
}

std::string Decoder::name(uint64_t va)
{
    if (!va)
        return "null";
    const uint8_t *host = map_ ? map_(va, 1) : nullptr;
    const uint64_t key = host ? uint64_t(uintptr_t(host)) : va;
    auto &n = names();
    auto it = n.find(key);
    if (it == n.end())
        it = n.emplace(key, int(n.size()) + 1).first;
    return "R" + std::to_string(it->second);
}

// The bytes of [va, va + bytes) with every GPU pointer replaced by the hash of what it points to (a descriptor's own size when
// it is an nvk_buffer_address {addr, size, 0}, else 64 bytes), and, in descriptor sets, every image/sampler handle by its
// texture/sampler header. Printed as a hash plus the decoded words.
std::string Decoder::contentHash(uint64_t va, uint64_t bytes, int depth, bool handles, bool childHandles)
{
    if (!bytes)
        return "empty";
    bytes = std::min<uint64_t>(bytes, 1 << 16);
    std::vector<uint8_t> b(bytes);
    if (!read(va, b.data(), bytes))
        return "unmapped:" + hex(bytes);
    std::string words;
    std::vector<bool> done(bytes / 4 + 1, false);
    if (handles && layout_) {  // a descriptor set: keep only the bytes its layout's entries cover
        std::vector<uint8_t> keep(bytes, 0);
        for (auto &r : layout_->ranges)
            for (uint32_t k = r.first; k < r.first + r.second && k < bytes; k++)
                keep[k] = 1;
        for (uint64_t k = 0; k < bytes; k++)
            if (!keep[k])
                b[k] = 0;
        if (getenv("NVDEC_SETWORDS")) {
            std::string l = "set:";
            for (uint64_t k = 0; k + 4 <= bytes && k < 96; k += 4) {
                uint32_t x;
                memcpy(&x, &b[k], 4);
                char s[12];
                snprintf(s, sizeof s, " %x", x);
                l += s;
            }
            words += " [" + l + "]";
        }
    }
    for (uint64_t i = 0; i + 8 <= bytes; i += 8) {
        uint64_t v;
        memcpy(&v, &b[i], 8);
        const uint64_t hi = v >> 32;
        if (hi < 1 || hi >= 0x80 || !map_ || !map_(v, 1) || depth >= 3)
            continue;
        uint32_t size = 64, z = 1;
        if (i % 16 == 0 && i + 16 <= bytes) {
            memcpy(&size, &b[i + 8], 4);
            memcpy(&z, &b[i + 12], 4);
            if (z != 0 || size == 0 || size > (1u << 16))
                size = 64;
        }
        const std::string inner = contentHash(v, size, depth + 1, childHandles, false);
        const std::string innerKey = nameless(inner);
        const uint64_t h = fnv(innerKey.data(), innerKey.size());
        memcpy(&b[i], &h, 8);
        done[i / 4] = done[i / 4 + 1] = true;
        words += " @" + std::to_string(i) + "=ptr{" + inner + "}";
        if (getenv("NVDEC_HEAD")) {
            uint32_t w[20] = {};
            read(v, w, 80);
            char t[96];
            snprintf(t, sizeof t, "[%08x %08x %08x %08x .. %08x]", w[0], w[1], w[2], w[3], w[16]);
            words += t;
        }
    }
    if (handles) {
        const uint32_t ticMax = r3(TIC_POOL_C), tscMax = r3(TSC_POOL_C);
        for (uint64_t i = 0; i + 4 <= bytes; i += 4) {
            if (done[i / 4])
                continue;
            uint32_t w;
            memcpy(&w, &b[i], 4);
            if (!w)
                continue;
            const uint32_t tic = w & 0xfffff, tsc = w >> 20;
            if (tic > ticMax || tsc > tscMax)
                continue;
            // NVK gives out sampler index 0 like any other: resolve it, unless the layout says the program reads no sampler there
            const bool noSampler = layout_ && layout_->textureOnly.count(uint32_t(i));
            const std::string t = tic ? ticEntry(tic) : "-", s = noSampler ? "-" : tscEntry(tsc);
            if (t == "zero" || s == "zero")
                continue;
            const std::string both = t + "/" + s, bothKey = nameless(both);
            const uint32_t h = uint32_t(fnv(bothKey.data(), bothKey.size()));
            memcpy(&b[i], &h, 4);
            words += " @" + std::to_string(i) + "=handle{" + both + "}";
        }
    }
    return hex(fnv(b.data(), b.size())) + ":" + hex(bytes) + words;
}

std::string Decoder::ticEntry(uint32_t index)
{
    uint32_t t[8];
    read(addr3(TIC_POOL_A, TIC_POOL_B) + uint64_t(index) * 32, t, 32);
    bool any = false;
    for (uint32_t w : t)
        any |= w != 0;
    if (!any)
        return "zero";
    const uint32_t version = (t[2] >> 21) & 7;
    const uint32_t mask = version == 3 || version == 4 ? 0xfffffe00u : version == 0 ? 0xffffffffu : 0xffffffe0u;
    const uint64_t address = (uint64_t(t[2] & 0xffff) << 32) | (t[1] & mask);
    // NVDEC_TEXELS: a pitch header's texels as a draw samples them, row by row (the image, wherever and however its rows lie:
    // a surface's planes and the pool image the copy engine fills from them hash the same)
    char texels[40] = "";
    static const bool wantTexels = getenv("NVDEC_TEXELS") != nullptr;
    if (wantTexels && version == 2) {
        uint32_t bpp = 0;
        switch (t[0] & 0x7f) {
        case 0x1d:
            bpp = 1;
            break;  // R8
        case 0x18:
            bpp = 2;
            break;  // G8R8
        case 0x08:
            bpp = 4;
            break;  // A8B8G8R8
        case 0x03:
            bpp = 8;
            break;  // R16G16B16A16
        case 0x01:
            bpp = 16;
            break;  // R32G32B32A32
        default:
            break;
        }
        const uint32_t pitch = (t[3] & 0xffff) << 5, w = (t[4] & 0xffff) + 1, h = (t[5] & 0xffff) + 1;
        if (bpp && pitch >= w * bpp && h <= 16384) {
            std::vector<uint8_t> row(size_t(w) * bpp);
            uint64_t hash = 0xcbf29ce484222325ull;
            for (uint32_t y = 0; y < h; y++) {
                if (!read(address + uint64_t(y) * pitch, row.data(), row.size()))
                    break;
                hash = fnv(row.data(), row.size(), hash);
            }
            snprintf(texels, sizeof texels, " texels=%016llx", (unsigned long long)hash);
        }
    }
    t[1] &= ~mask;
    t[2] &= ~0xffffu;
    char b[200];
    snprintf(b, sizeof b, "tic[%s %08x %08x %08x %08x %08x %08x %08x %08x%s]", name(address).c_str(), t[0], t[1], t[2],
             t[3], t[4], t[5], t[6], t[7], texels);
    return b;
}

std::string Decoder::tscEntry(uint32_t index)
{
    uint32_t t[8];
    read(addr3(TSC_POOL_A, TSC_POOL_B) + uint64_t(index) * 32, t, 32);
    bool any = false;
    for (uint32_t w : t)
        any |= w != 0;
    if (!any)
        return "zero";
    char b[120];
    snprintf(b, sizeof b, "tsc[%08x %08x %08x %08x %08x %08x %08x %08x]", t[0], t[1], t[2], t[3], t[4], t[5], t[6],
             t[7]);
    return b;
}

bool Decoder::loadLayouts(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        std::vector<std::string> w;
        for (char *tok = strtok(line, " \n"); tok; tok = strtok(nullptr, " \n"))
            w.push_back(tok);
        if (w.size() < 3)
            continue;
        Layout l;
        l.bytes = uint32_t(strtoul(w[2].c_str(), nullptr, 16));
        for (size_t i = 3; i < w.size(); i++) {
            const uint32_t off = uint32_t(strtoul(w[i].c_str(), nullptr, 10));
            l.ranges.push_back({off, w[i].back() == 'u' ? 16u : 4u});
            if (w[i].back() == 't')
                l.textureOnly.insert(off);
        }
        layouts[w[0] + " " + w[1]] = l;
    }
    fclose(f);
    return true;
}

bool Decoder::loadPrograms(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    std::vector<uint8_t> b;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        b.insert(b.end(), chunk, chunk + n);
    fclose(f);
    for (size_t at = 0; at + 16 <= b.size();) {
        uint32_t code, count, data;
        if (memcmp(&b[at], "NVPG", 4))
            return false;
        memcpy(&code, &b[at + 4], 4);
        memcpy(&count, &b[at + 8], 4);
        memcpy(&data, &b[at + 12], 4);
        at += 16;
        ProgramInfo p;
        p.cbufTypes.assign(b.begin() + at, b.begin() + at + count);
        at += count;
        p.image.assign(b.begin() + at, b.begin() + at + 80 + code);
        at += 80 + code;
        p.data.assign(b.begin() + at, b.begin() + at + data);
        at += data;
        p.id = hex(fnv(p.data.data(), p.data.size(), fnv(p.image.data(), p.image.size())));
        programs.emplace(fnv(p.image.data(), 80), p);
    }
    return true;
}

// The program a stage runs (pre-Volta: at SET_PIPELINE_PROGRAM bytes into SET_PROGRAM_REGION): a known one when its header,
// code and shader data match exactly, named by the hash of all three; else the hash of its header and first 256 bytes of code,
// marked unknown. The header sits 0x30 bytes into its upload and the data 256-aligned after the code (nvk_shader_upload).
const ProgramInfo *Decoder::program(int stage, std::string &id)
{
    const uint32_t offset = r3(PIPELINE + 0x40 * stage + 4);
    const uint64_t at = addr3(PROGRAM_REGION_A, PROGRAM_REGION_B) + offset;
    uint8_t head[80 + 256];
    if (!read(at, head, sizeof head)) {
        id = "unmapped";
        return nullptr;
    }
    auto match = [&]() -> const ProgramInfo * {
        auto range = programs.equal_range(fnv(head, 80));
        for (auto it = range.first; it != range.second; ++it) {
            const ProgramInfo &p = it->second;
            std::vector<uint8_t> b(p.image.size()), data(p.data.size());
            read(at, b.data(), b.size());
            const uint64_t dataAt = at - 0x30 + ((0x30 + b.size() + 0xff) & ~uint64_t(0xff));
            if (b == p.image && (data.empty() || (read(dataAt, data.data(), data.size()) && data == p.data)))
                return &p;
        }
        return nullptr;
    };
    const ProgramInfo *known = match();
    ProgramInfo learned;
    if (!known && programAt && programAt(offset, learned) && learned.image.size() >= 80) {
        learned.id =
            hex(fnv(learned.data.data(), learned.data.size(), fnv(learned.image.data(), learned.image.size())));
        programs.emplace(fnv(learned.image.data(), 80), learned);
        known = match();
    }
    if (known) {
        id = known->id;
        return known;
    }
    id = "unknown:" + hex(fnv(head, sizeof head));
    if (describe && getenv("NVDEC_DESCRIBE")) {
        fprintf(stderr, "program stage %d at ", stage);
        describe(at);
    }
    if (const char *dir =
            getenv("NVDEC_DUMPPROG")) {  // the unknown program's header and first 8 KiB, for a diff with known ones
        std::vector<uint8_t> b(80 + 8192);
        read(at, b.data(), b.size());
        const std::string path = std::string(dir) + "/" + id.substr(8) + ".bin";
        if (FILE *f = fopen(path.c_str(), "wb")) {
            fwrite(b.data(), 1, b.size(), f);
            fclose(f);
        }
    }
    return nullptr;
}

void Decoder::clear(uint32_t data)
{
    clears++;
    countComp(data);
    std::string bar;
    for (auto &b : pendingBarriers_)
        bar += (bar.empty() ? "" : ",") + b;
    pendingBarriers_.clear();
    const uint32_t rt = (data >> 6) & 0xf;
    const uint32_t m = COLOR_TARGET + 0x40 * rt;
    // depth (Z_ENABLE) or stencil (STENCIL_ENABLE): their clear values and the depth target too
    char zs[96] = "";
    if (data & 3)
        snprintf(zs, sizeof zs, " zs=%08x,%02x,%s", r3(Z_CLEAR_VALUE), r3(STENCIL_CLEAR_VALUE) & 0xff,
                 name(addr3(ZT_A, ZT_B)).c_str());
    fprintf(
        out_,
        "CLEAR data=%08x rt=%s,%ux%u,fmt=%x,mem=%x color=%08x,%08x,%08x,%08x rect=%08x,%08x ctl=%08x%s barriers=%s\n",
        data, name(addr3(m, m + 4)).c_str(), r3(m + 8), r3(m + 0xc), r3(m + 0x10), r3(m + 0x14), r3(COLOR_CLEAR_VALUE),
        r3(COLOR_CLEAR_VALUE + 4), r3(COLOR_CLEAR_VALUE + 8), r3(COLOR_CLEAR_VALUE + 0xc), r3(CLEAR_RECT_H),
        r3(CLEAR_RECT_V), r3(CLEAR_SURFACE_CONTROL), zs, bar.c_str());
}

// A draw (clearData 0) or a clear into a compressed target (nvdec_comp.h)
void Decoder::countComp(uint32_t clearData)
{
    if (!vaKind)
        return;
    const uint32_t rt = (clearData >> 6) & 0xf, m = COLOR_TARGET + 0x40 * rt;
    const uint32_t colorKind = r3(COLOR_COMPRESSION + 4 * rt) ? vaKind(addr3(m, m + 4)) : 0;
    const uint32_t zetaKind = (r3(ZT_SELECT) & 1) && r3(Z_COMPRESSION) ? vaKind(addr3(ZT_A, ZT_B)) : 0;
    auto add = [&](uint64_t CompCounts::*f, uint64_t n) {
        comp.*f += n;
        compSubmit.*f += n;
    };
    if (!clearData) {
        uint32_t live = r3(CT_SELECT) & 0xf;
        bool any = compressibleKind(zetaKind);
        for (uint32_t j = 0; j < live && j < 8 && !any; j++)
            any = r3(COLOR_COMPRESSION + 4 * j) &&
                  compressibleKind(vaKind(addr3(COLOR_TARGET + 0x40 * j, COLOR_TARGET + 0x40 * j + 4)));
        if (any)
            add(&CompCounts::draws, 1);
        return;
    }
    const bool rect = r3(CLEAR_SURFACE_CONTROL) & 0x10;
    const uint32_t h = r3(CLEAR_RECT_H), v = r3(CLEAR_RECT_V);
    auto whole = [&](uint32_t width, uint32_t height) {
        return !rect || (!(h & 0xffff) && !(v & 0xffff) && (h >> 16) >= width && (v >> 16) >= height);
    };
    if ((clearData & 0x3c) && compressibleKind(colorKind)) {
        const uint32_t w[4] = {r3(COLOR_CLEAR_VALUE), r3(COLOR_CLEAR_VALUE + 4), r3(COLOR_CLEAR_VALUE + 8),
                               r3(COLOR_CLEAR_VALUE + 0xc)};
        add(&CompCounts::clears, 1);
        if ((clearData & 0x3c) == 0x3c && whole(r3(m + 8), r3(m + 0xc)) && zbcBootColorNv(r3(m + 0x10), w)) {
            add(&CompCounts::zbc, 1);
            add(&CompCounts::zbcBytes, uint64_t(r3(m + 8)) * r3(m + 0xc) * compBytesPerTexel(colorKind));
        }
    }
    if ((clearData & 1) && compressibleKind(zetaKind)) {
        add(&CompCounts::clears, 1);
        if (whole(r3(ZT_SIZE_A), r3(ZT_SIZE_A + 4)) && zbcBootDepthBits(r3(Z_CLEAR_VALUE))) {
            add(&CompCounts::zbc, 1);
            add(&CompCounts::zbcBytes, uint64_t(r3(ZT_SIZE_A)) * r3(ZT_SIZE_A + 4) * compBytesPerTexel(zetaKind));
        }
    }
}

// Raw facts of a draw for the pack generator (tools/mkpack.py) and the runtime's record maker: every non-zero 3D register, the
// bound constant buffers, the root table's words, the sets' words, the words behind each pointer in them, the texture/sampler
// headers set 0 can name and the pools' first entries, the vertex streams and the programs.
void Decoder::probeRecord(ProbeRecord &r)
{
    r = {};
    for (uint32_t i = 0x200 / 4; i < 0x4000; i++)
        if (reg_[0][i] && i * 4 < MME_SCRATCH)
            r.regs[i * 4] = reg_[0][i];
    auto words = [&](uint64_t va, uint32_t bytes) {
        ProbeRecord::Words w{va, std::vector<uint32_t>(bytes / 4)};
        read(va, w.words.data(), bytes);
        return w;
    };
    auto pointer = [&](uint64_t v) {
        return (v >> 32) >= 1 && (v >> 32) < 0x80 && map_ && map_(v, 1);
    };
    for (int g = 0; g < 5; g++)
        for (int s = 0; s < 18; s++)
            if (binds_[g][s].valid)
                r.binds.push_back({g, s, binds_[g][s].addr, binds_[g][s].size});
    const uint64_t root = binds_[4][0].valid ? binds_[4][0].addr : binds_[0][0].addr;
    r.root = words(root, 0x780);
    // the root-table words written this submission in the push windows [0x700, 0x780): m2v's push ranges as NVK wrote them
    for (auto it = writtenThisSubmit_.lower_bound(root + 0x700); it != writtenThisSubmit_.end() && *it < root + 0x780;
         ++it)
        r.pushWords.push_back({uint32_t(*it - root), cbufWrites_[*it]});
    const std::vector<uint32_t> &rw = r.root.words;
    for (int set = 0; set < 2; set++) {
        const uint64_t sa = (uint64_t(rw[(0x38 + 16 * set) / 4 + 1]) << 32) | rw[(0x38 + 16 * set) / 4];
        const uint32_t ss = rw[(0x38 + 16 * set) / 4 + 2];
        if (!sa || !ss)
            continue;
        r.sets[set] = words(sa, std::min<uint32_t>(ss, 0x1000));
        const std::vector<uint32_t> &d = r.sets[set].words;
        for (size_t i = 0; i + 3 < d.size(); i += 2) {
            const uint64_t v = (uint64_t(d[i + 1]) << 32) | d[i];
            if (pointer(v))
                r.setPointers[set][uint32_t(i * 4)] = words(v, 256);
        }
    }
    for (uint32_t i = 0x700 / 4; i + 1 < 0x700 / 4 + 32; i += 2) {
        const uint64_t v = (uint64_t(rw[i + 1]) << 32) | rw[i];
        if (pointer(v))
            r.pushPointers[(i - 0x700 / 4) * 4] = words(v, 256);
    }
    // the texture and sampler headers any set-0 word could name (tic in bits 19:0, tsc in 31:20), plus entry 0 of each pool
    std::set<uint32_t> tics = {0}, tscs = {0};
    for (uint32_t w : r.sets[0].words) {
        if ((w & 0xfffff) <= r3(TIC_POOL_C))
            tics.insert(w & 0xfffff);
        if ((w >> 20) <= r3(TSC_POOL_C))
            tscs.insert(w >> 20);
    }
    auto header = [](const uint32_t (&e)[8]) {
        std::array<uint32_t, 8> a;
        std::copy(e, e + 8, a.begin());
        return a;
    };
    for (uint32_t t : tics) {
        uint32_t e[8];
        read(addr3(TIC_POOL_A, TIC_POOL_B) + 32ull * t, e, 32);
        if (std::any_of(e, e + 8, [](uint32_t x) {
                return x != 0;
            }))
            r.tic[t] = {name((uint64_t(e[2] & 0xffff) << 32) | (e[1] & 0xffffffe0u)), header(e)};
    }
    for (uint32_t t : tscs) {
        uint32_t e[8];
        read(addr3(TSC_POOL_A, TSC_POOL_B) + 32ull * t, e, 32);
        if (std::any_of(e, e + 8, [](uint32_t x) {
                return x != 0;
            }))
            r.tsc[t] = header(e);
    }
    for (uint32_t j = 0; j < 16; j++) {
        const uint32_t fmt = r3(VERTEX_STREAM_A + 0x10 * j);
        if (!(fmt & (1u << 12)))
            continue;
        ProbeRecord::Stream &s = r.streams[j];
        s.format = fmt;
        s.location = addr3(VERTEX_STREAM_A + 0x10 * j + 4, VERTEX_STREAM_A + 0x10 * j + 8);
        s.limit = addr3(VERTEX_STREAM_LIMIT + 8 * j, VERTEX_STREAM_LIMIT + 8 * j + 4);
        s.words = words(s.location, 256).words;
    }
    for (int i = 0; i < 6; i++) {
        if (!(r3(PIPELINE + 0x40 * i) & 1))
            continue;
        ProbeRecord::Program &p = r.programs[i];
        p.known = program(i, p.id) != nullptr;
        p.offset = r3(PIPELINE + 0x40 * i + 4);
    }
    r.begin = r3(BEGIN);
    const uint32_t ct = r3(CT_SELECT);
    for (uint32_t k = 0; k < (ct & 0xf) && k < 8; k++) {
        const uint32_t j = (ct >> (4 + 3 * k)) & 7, m = COLOR_TARGET + 0x40 * j;
        r.targets[name(addr3(m, m + 4))] = j;
    }
}

namespace {

void printWords(FILE *out, const std::string &tag, const ProbeRecord::Words &w)
{
    std::string l = "P " + tag + " " + hex(w.va);
    for (uint32_t x : w.words) {
        char b[12];
        snprintf(b, sizeof b, " %x", x);
        l += b;
    }
    fprintf(out, "%s\n", l.c_str());
}

void printProbe(FILE *out, const ProbeRecord &r)
{
    fprintf(out, "P begin\n");
    std::string line = "P regs";
    for (const auto &[m, v] : r.regs) {
        char b[24];
        snprintf(b, sizeof b, " %x=%x", m, v);
        line += b;
    }
    fprintf(out, "%s\n", line.c_str());
    for (const ProbeRecord::Bind &b : r.binds)
        fprintf(out, "P bind %d %d %s %x\n", b.group, b.slot, hex(b.addr).c_str(), b.bytes);
    printWords(out, "root", r.root);
    std::string l = "P pushw";
    for (const auto &[offset, word] : r.pushWords) {
        char b[32];
        snprintf(b, sizeof b, " %x=%x", offset, word);
        l += b;
    }
    fprintf(out, "%s\n", l.c_str());
    for (int set = 0; set < 2; set++) {
        if (r.sets[set].words.empty())
            continue;
        printWords(out, "set" + std::to_string(set), r.sets[set]);
        for (const auto &[offset, w] : r.setPointers[set])
            printWords(out, "ptr" + std::to_string(set) + "." + std::to_string(offset), w);
    }
    for (const auto &[offset, w] : r.pushPointers)
        printWords(out, "pushptr." + std::to_string(offset), w);
    for (const auto &[t, h] : r.tic)
        fprintf(out, "P tic %u %s %x %x %x %x %x %x %x %x\n", t, h.name.c_str(), h.w[0], h.w[1], h.w[2], h.w[3], h.w[4],
                h.w[5], h.w[6], h.w[7]);
    for (const auto &[t, e] : r.tsc)
        fprintf(out, "P tsc %u %x %x %x %x %x %x %x %x\n", t, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
    for (const auto &[j, s] : r.streams) {
        fprintf(out, "P vbinfo %u %s %s %x\n", j, hex(s.location).c_str(), hex(s.limit).c_str(), s.format);
        printWords(out, "vb" + std::to_string(j), {s.location, s.words});
    }
    for (const auto &[i, p] : r.programs)
        fprintf(out, "P prog %d %s %x %s\n", i, p.id.c_str(), p.offset, p.known ? "known" : "unknown");
    fprintf(out, "P draw %u %u %x\n", r.first, r.count, r.begin);
    fprintf(out, "P end\n");
}

}  // namespace

void Decoder::probeDump(uint32_t first, uint32_t count)
{
    ProbeRecord r;
    probeRecord(r);
    r.first = first;
    r.count = count;
    printProbe(out_, r);
    // NVDEC_SCRUB: zero set 0 and what its pointers reach once read, so that the next draw's descriptors and payloads show only
    // the bytes m2v writes for it (its allocations are recycled: what it does not write would otherwise be the last draw's)
    if (scrub && getenv("NVDEC_SCRUB")) {
        if (const std::vector<uint32_t> &d = r.sets[0].words; !d.empty()) {
            for (size_t i = 0; i + 1 < d.size(); i += 2) {
                const uint64_t v = (uint64_t(d[i + 1]) << 32) | d[i];
                if ((v >> 32) >= 1 && (v >> 32) < 0x80 && map_ && map_(v, 1))
                    scrub(v, 256);
            }
            scrub(r.sets[0].va, d.size() * 4);
        }
        for (const auto &[j, s] : r.streams)
            scrub(s.location, 256);
    }
}

const ProgramInfo *Decoder::programById(const std::string &id) const
{
    for (const auto &kv : programs)
        if (kv.second.id == id)
            return &kv.second;
    return nullptr;
}

bool Decoder::bound(int group, int slot, uint64_t &addr, uint32_t &bytes) const
{
    if (group < 0 || group > 4 || slot < 0 || slot >= 18 || !binds_[group][slot].valid)
        return false;
    addr = binds_[group][slot].addr;
    bytes = binds_[group][slot].size;
    return true;
}

void Decoder::pushWords(uint32_t &mask, uint32_t words[32]) const
{
    mask = 0;
    const uint64_t root = binds_[4][0].valid ? binds_[4][0].addr : binds_[0][0].addr;
    for (auto it = writtenThisSubmit_.lower_bound(root + 0x700); it != writtenThisSubmit_.end() && *it < root + 0x780;
         ++it) {
        const uint32_t k = uint32_t(*it - root - 0x700) / 4;
        mask |= 1u << k;
        words[k] = cbufWrites_.at(*it);
    }
}

// Whether a live stage's program reads a constant buffer (not the root table, which LOAD_CONSTANT_BUFFER writes in order) over
// bytes in constantStale_
bool Decoder::readsStaleConstants()
{
    for (int i = 0; i < 6; i++) {
        const uint32_t group = r3(PIPELINE + 0x40 * i + 0x10);
        if (!(r3(PIPELINE + 0x40 * i) & 1) || group >= 5)
            continue;
        std::string id;
        const ProgramInfo *info = program(i, id);
        const size_t slots = info ? info->cbufTypes.size() : 18;
        for (size_t s = 0; s < slots; s++) {
            const Bind &b = binds_[group][s];
            const uint8_t type = info ? info->cbufTypes[s] & 7 : 0;
            if (!b.valid || type == 1 || (type == 0 && s == 0))
                continue;
            for (const auto &r : constantStale_)
                if (b.addr < r.second && r.first < b.addr + b.size)
                    return true;
        }
    }
    return false;
}

void Decoder::draw(const char *kind, uint32_t first, uint32_t count)
{
    draws++;
    if (onDraw)
        onDraw(*this);
    if (!constantStale_.empty() && readsStaleConstants()) {
        staleConstantReads++;
        barrier("CBSTALE");
    }
    std::string bar;
    for (auto &b : pendingBarriers_)
        bar += (bar.empty() ? "" : ",") + b;
    pendingBarriers_.clear();
    fprintf(out_, "%s first=%u count=%u begin=%08x barriers=%s\n", indexed_ ? "DRAW-INDEXED" : kind, first, count,
            r3(BEGIN), bar.c_str());
    countComp(0);

    // Which render targets, vertex streams and stages are live: their registers are compared, the others are not.
    const uint32_t ct = r3(CT_SELECT), ctCount = ct & 0xf;
    bool rtLive[8] = {};
    for (uint32_t k = 0; k < ctCount && k < 8; k++)
        rtLive[(ct >> (4 + 3 * k)) & 7] = true;
    bool stageLive[6] = {};
    for (int i = 0; i < 6; i++)
        stageLive[i] = r3(PIPELINE + 0x40 * i) & 1;
    auto live = [&](uint32_t m) {
        if (m >= COLOR_TARGET && m < COLOR_TARGET + 0x200)
            return rtLive[(m - COLOR_TARGET) / 0x40];
        if (m >= COLOR_COMPRESSION && m < COLOR_COMPRESSION + 0x20)
            return rtLive[(m - COLOR_COMPRESSION) / 4];
        if (m >= VERTEX_STREAM_A && m < VERTEX_STREAM_A + 0x200) {
            const uint32_t j = (m - VERTEX_STREAM_A) / 0x10;
            return bool(r3(VERTEX_STREAM_A + 0x10 * j) & (1u << 12));
        }
        if (m >= 0x1880 && m < 0x18c0)
            return bool(r3(VERTEX_STREAM_A + 0x10 * ((m - 0x1880) / 4)) & (1u << 12));
        if (m >= PIPELINE && m < PIPELINE + 0x180) {
            const uint32_t i = (m - PIPELINE) / 0x40;
            return (m & 0x3f) == 0 || stageLive[i];
        }
        // the primitive restart index is the index buffer's: a vertex array draw reads none (NVK leaves its last index
        // buffer's in place or puts its pipeline's 0 back, as its dynamic state goes, before either)
        if (m == 0x1648)
            return indexed_;
        return true;
    };

    if (firstDraw_ && fullStateAtFirstDraw) {
        for (uint32_t i = 0x200 / 4; i < 0x4000; i++) {
            const uint32_t m = i * 4;
            if (!ignored3d(m) && live(m) && reg_[0][i])
                fprintf(out_, "  STATE %04x=%08x\n", m, reg_[0][i]);
        }
    }
    firstDraw_ = false;

    if (getenv("NVDEC_PROBE"))
        probeDump(first, count);
    // every live, comparable register (the record differs between two streams iff the GPU state they leave differs)
    std::string regs;
    uint64_t regHash = 0xcbf29ce484222325ull;
    for (uint32_t i = 0x200 / 4; i < 0x4000; i++) {
        const uint32_t m = i * 4;
        if (ignored3d(m) || !live(m) || !reg_[0][i])
            continue;
        regHash = fnv(&m, 4, regHash);
        regHash = fnv(&reg_[0][i], 4, regHash);
    }
    fprintf(out_, "  regs=%s\n", hex(regHash).c_str());
    if (getenv("NVDEC_REGS")) {  // every live comparable register, for tools/eqcheck.py to name what differs
        std::string l = "  R";
        for (uint32_t i = 0x200 / 4; i < 0x4000; i++) {
            const uint32_t m = i * 4;
            if (ignored3d(m) || !live(m) || !reg_[0][i])
                continue;
            char b[24];
            snprintf(b, sizeof b, " %x=%x", m, reg_[0][i]);
            l += b;
        }
        fprintf(out_, "%s\n", l.c_str());
        // sampler header 0, which a handle naming no sampler names: NVK's texel-fetch sampler on every GPU (nvk_device.c)
        fprintf(out_, "  tsc0=%s\n", tscEntry(0).c_str());
    }
    // render targets and depth target by name
    for (uint32_t j = 0; j < 8; j++) {
        if (!rtLive[j])
            continue;
        const uint32_t m = COLOR_TARGET + 0x40 * j;
        fprintf(out_, "  rt%u=%s %ux%u fmt=%x mem=%x pitch=%x\n", j, name(addr3(m, m + 4)).c_str(), r3(m + 8),
                r3(m + 0xc), r3(m + 0x10), r3(m + 0x14), r3(m + 0x1c));
    }
    if (r3(ZT_SELECT) & 1)
        fprintf(out_, "  zt=%s\n", name(addr3(ZT_A, ZT_B)).c_str());
    // vertex streams: the bytes the draw reads
    for (uint32_t j = 0; j < 16; j++) {
        const uint32_t fmt = r3(VERTEX_STREAM_A + 0x10 * j);
        if (!(fmt & (1u << 12)))
            continue;
        const uint64_t loc = addr3(VERTEX_STREAM_A + 0x10 * j + 4, VERTEX_STREAM_A + 0x10 * j + 8);
        const uint64_t limit = addr3(VERTEX_STREAM_LIMIT + 8 * j, VERTEX_STREAM_LIMIT + 8 * j + 4);
        const uint32_t stride = fmt & 0xfff;
        uint64_t bytes = uint64_t(first + count) * stride;
        if (limit >= loc)
            bytes = std::min<uint64_t>(bytes, limit - loc + 1);
        uint32_t head[4] = {};
        read(loc, head, 16);
        if (describe && getenv("NVDEC_DESCRIBE"))
            describe(loc);
        fprintf(out_, "  vb%u stride=%u data=%s head=%08x,%08x,%08x,%08x\n", j, stride,
                contentHash(loc, bytes, 2, false, false).c_str(), head[0], head[1], head[2], head[3]);
    }
    // the draw's set-0 layout, by its program pair
    layout_ = nullptr;
    {
        std::string vs, fs;
        if (stageLive[1])
            program(1, vs);
        if (stageLive[5])
            program(5, fs);
        auto it = layouts.find(vs + " " + fs);
        if (it != layouts.end())
            layout_ = &it->second;
        else if (!layouts.empty())
            fprintf(out_, "  layout unknown\n");
    }
    // programs and the constant buffers each live stage's program uses
    for (int i = 0; i < 6; i++) {
        if (!stageLive[i])
            continue;
        std::string prog;
        const ProgramInfo *info = program(i, prog);
        const uint32_t group = r3(PIPELINE + 0x40 * i + 0x10);
        fprintf(out_, "  stage%d prog=%s regs=%u group=%u\n", i, prog.c_str(), r3(PIPELINE + 0x40 * i + 0xc), group);
        if (group >= 5)
            continue;
        const size_t slots = info ? info->cbufTypes.size() : 18;
        for (size_t s = 0; s < slots; s++) {
            const Bind &b = binds_[group][s];
            const uint8_t type = info ? info->cbufTypes[s] & 7 : 0;  // raw_pack.h PackCbufType
            if (!b.valid) {
                if (info)
                    fprintf(out_, "    cb%zu INVALID\n", s);
                continue;
            }
            if (type == 1 || (type == 0 && s == 0)) {
                // root table: draw parameters, set addresses, push constants (nvk_root_descriptor_table)
                uint8_t draw[0x38];
                read(b.addr, draw, sizeof draw);
                // sample_locations[1..7] (NVK uploads 8 bytes of which a 1-sample layout sets one) and sample_masks[] (read only by
                // sample-rate shading; no Core Animation program has it, nak_shader_info.fs): not compared
                memset(draw + 0x1d, 0, 0x34 - 0x1d);
                fprintf(out_, "    cb%zu root draw=%s\n", s, hex(fnv(draw, sizeof draw)).c_str());
                for (uint32_t set = 0; set < 2; set++)
                    fprintf(out_, "    cb%zu root set%u=%s\n", s, set,
                            contentHash(b.addr + 0x38 + 16 * set, 16, 0, false, true).c_str());
                fprintf(out_, "    cb%zu root push=%s\n", s,
                        contentHash(b.addr + 0x700, 0x80, 1, false, false).c_str());
                if (getenv("NVDEC_ROOTWORDS")) {
                    uint32_t w[0x780 / 4];
                    read(b.addr, w, sizeof w);
                    std::string l = "    rootwords draw:";
                    for (int k = 0; k < 14; k++) {
                        char x[12];
                        snprintf(x, sizeof x, " %x", w[k]);
                        l += x;
                    }
                    l += " push:";
                    for (int k = 0x700 / 4; k < 0x780 / 4; k++) {
                        char x[12];
                        snprintf(x, sizeof x, " %x", w[k]);
                        l += x;
                    }
                    fprintf(out_, "%s\n", l.c_str());
                }
            } else if (type == kSplitFrontier) {
                const auto launch = splitLaunches_.find(b.addr);
                fprintf(out_, "    cb%zu frontier size=%x %s\n", s, b.size,
                        launch == splitLaunches_.end() ? "written by no prologue" : launch->second.c_str());
            } else {
                fprintf(out_, "    cb%zu type=%u size=%x data=%s\n", s, type, b.size,
                        contentHash(b.addr, b.size, type == 3 ? 1 : 2, type == 3, false).c_str());
            }
        }
    }
}

}  // namespace nvdec
