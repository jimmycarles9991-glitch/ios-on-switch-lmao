// The records the bench draws and dispatches with: a pack state's vertex program, registers and set-0 entries with the
// caller's pixel program in place of its own, and a pack kernel's QMD around the caller's kernel.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_compute_record.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_record.h"

namespace bench {

using Methods = std::vector<std::pair<uint32_t, uint32_t>>;

struct Program {
    std::vector<uint8_t> sph, code, data;
    uint32_t gprs = 0;
    uint32_t dataBound = 0;  // the bound size of the data's constant buffer; 0: padded to 256-byte steps
};

// The plainest single-sample state of the pack whose first colour is written whole and unblended, whose vertex program
// reads a float4 position, and which fetches, stores and tests nothing: its pixel program is the one replaced.
const raw::PackPipeline *fragmentTemplate(const raw::Pack &pack);
// The pack state keyed `key` when it is as plain as fragmentTemplate's choice must be, else null
const raw::PackPipeline *fragmentTemplate(const raw::Pack &pack, uint64_t key);
// fragmentTemplate's choice among the states whose set-0 entries include a texture with a sampler slot: the template of a
// program that samples (its bound TEX/TLD read that texture's handle), null if the pack has none
const raw::PackPipeline *textureTemplate(const raw::Pack &pack);
// A bound TEX or TLD whose texture index is this reads the template's texture: fragmentRecord puts its handle's index in
constexpr uint32_t kTextureIndexPlaceholder = 0x1fff;
bool samplesTexture(const Program &p);
// A pack kernel whose program's constant buffer 0 is the root table: its QMD is the one patched.
const raw::PackKernel *kernelTemplate(const raw::Pack &pack);

// `tpl` with `fs` as its pixel program, its first colour of `format` with all four channels written, `methods` set with
// its registers and `restore` set when the next state is drawn; keyed `key`, which no pack state has. `firstColourOnly`
// drops the template's other colours from the state, for a pass that binds colour 0 alone. False: `why`.
bool fragmentRecord(const raw::Pack &pack, const raw::PackPipeline &tpl, const Program &fs, uint32_t format,
                    uint64_t key, const Methods &methods, const Methods &restore, raw::RuntimeRecord &r,
                    std::string &why, bool firstColourOnly = false);

// A state key without its colours 1..7
std::string withoutOtherColours(std::string key);
bool kernelRecord(const raw::PackKernel &tpl, const Program &kernel, const uint32_t threads[3], raw::ComputeRecord &r,
                  std::string &why);

// Methods a caller may set around its draw: never one the raw path owns (raw_owned.h), only the ROP, blend, iterated blend
// and colour write state of tools/benchloop/protocol.py METHOD_RANGES
bool methodAllowed(uint32_t method);

}  // namespace bench
