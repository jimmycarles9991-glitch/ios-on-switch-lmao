// SPDX-License-Identifier: MIT
// nvdec (r7-raw): see nvdec.h. Method numbers: NVIDIA's class headers as Mesa ships them, through kits/SwitchGM20B (MIT).
// Macros run through Mesa's Fermi MME simulator (src/nouveau/mme/mme_fermi_sim.c, MIT; third_party/mesa/libnouveau_mme.a).
#include "nvdec.h"
#include "nvdec_methods.h"
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

// An indexed draw of at most this many indices prints them by value (eqcheck.py joins a split draw's runs from them)
constexpr uint32_t kListedIndices = 256;

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
        // gpu.cpp's marks (SWITCH_METAL_HARNESS_MARKS builds): a fetch barrier left out, a draw's run after its first
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

}  // namespace nvdec
