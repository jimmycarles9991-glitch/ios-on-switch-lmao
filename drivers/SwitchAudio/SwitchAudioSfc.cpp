// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchAudioAhub.h"

namespace switch_audio_ahub {
namespace {

constexpr uint32_t kTimeoutUs = 10000u;

// Tegra210 SFC Q23 coefficient RAM programs (mainline sound/soc/tegra/tegra210_sfc.c), zero-padded to 64 words.
constexpr uint32_t kCoef2xDec[kSfcCoefDepth] = {
    0x00005102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu, 0x001c564cu, 0x00e55557u,
    0xffcadd5bu, 0x003d80bau, 0x00d13397u, 0xfff232f8u, 0x00683337u, 0x00000001u,
};

constexpr uint32_t kCoef2xInt[kSfcCoefDepth] = {
    0x00006102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu, 0x001c564cu, 0x00e55557u,
    0xffcadd5bu, 0x003d80bau, 0x00d13397u, 0xfff232f8u, 0x00683337u, 0x00000002u,
};

constexpr uint32_t kCoef4xDec[kSfcCoefDepth] = {
    0x000c5102u, 0x000013d9u, 0x00ebd477u, 0xff4ce383u, 0x0042049du, 0x0089c278u, 0xff54414du, 0x00531dedu,
    0x004a5e07u, 0xff53cf41u, 0x006efbdcu, 0x00000001u, 0x00005102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu,
    0x001c564cu, 0x00e55557u, 0xffcadd5bu, 0x003d80bau, 0x00d13397u, 0xfff232f8u, 0x00683337u, 0x00000001u,
};

constexpr uint32_t kCoef4xInt[kSfcCoefDepth] = {
    0x000c6102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu, 0x001c564cu, 0x00e55557u, 0xffcadd5bu, 0x003d80bau,
    0x00d13397u, 0xfff232f8u, 0x00683337u, 0x00000002u, 0x00006102u, 0x000013d9u, 0x00ebd477u, 0xff4ce383u,
    0x0042049du, 0x0089c278u, 0xff54414du, 0x00531dedu, 0x004a5e07u, 0xff53cf41u, 0x006efbdcu, 0x00000002u,
};

constexpr uint32_t kCoef44100To48000[kSfcCoefDepth] = {
    0x000c6102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu, 0x001c564cu, 0x00e55557u, 0xffcadd5bu, 0x003d80bau,
    0x00d13397u, 0xfff232f8u, 0x00683337u, 0x00000002u, 0x00186102u, 0x000013d9u, 0x00ebd477u, 0xff4ce383u,
    0x0042049du, 0x0089c278u, 0xff54414du, 0x00531dedu, 0x004a5e07u, 0xff53cf41u, 0x006efbdcu, 0x00000002u,
    0x00235204u, 0x000aaaabu, 0xffaaaaabu, 0xfffaaaabu, 0x00555555u, 0xff600000u, 0xfff55555u, 0x00155555u,
    0x00055555u, 0xffeaaaabu, 0x00200000u, 0x00005102u, 0x0001d029u, 0x00f2a98bu, 0xff92aa71u, 0x001fcd16u,
    0x00ae9004u, 0xffb85140u, 0x0041813au, 0x007f8ed1u, 0xffd585fcu, 0x006a69e6u, 0x00000001u,
};

constexpr uint32_t kCoef48000To44100[kSfcCoefDepth] = {
    0x000c6102u, 0x0001d029u, 0x00f2a98bu, 0xff92aa71u, 0x001fcd16u, 0x00ae9004u, 0xffb85140u, 0x0041813au, 0x007f8ed1u,
    0xffd585fcu, 0x006a69e6u, 0x00000002u, 0x001b6103u, 0x000001e0u, 0x00de44c0u, 0xff380b7fu, 0x004ffc73u, 0x00494b44u,
    0xff3d493au, 0x005908bfu, 0xffe9a3c8u, 0xff425647u, 0x006745f7u, 0xffc42d61u, 0xff40a6c7u, 0x00776709u, 0x00000002u,
    0x00265204u, 0x000aaaabu, 0xffaaaaabu, 0xfffaaaabu, 0x00555555u, 0xff600000u, 0xfff55555u, 0x00155555u, 0x00055555u,
    0xffeaaaabu, 0x00200000u, 0x00005102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu, 0x001c564cu, 0x00e55557u, 0xffcadd5bu,
    0x003d80bau, 0x00d13397u, 0xfff232f8u, 0x00683337u, 0x00000001u,
};

constexpr uint32_t kCoef16000To48000[kSfcCoefDepth] = {
    0x0000a105u, 0x00000784u, 0x00cc516eu, 0xff2c9639u, 0x005ad5b3u, 0x0013ad0du, 0xff3d4799u,
    0x0063ce75u, 0xffb6f398u, 0xff5138d1u, 0x006e9e1fu, 0xff9186e5u, 0xff5f96a4u, 0x0076a86eu,
    0xff82089cu, 0xff676b81u, 0x007b9f8au, 0xff7c48a5u, 0xff6a31e7u, 0x007ebb7bu, 0x00000003u,
};

constexpr uint32_t kCoef48000To16000[kSfcCoefDepth] = {
    0x00009105u, 0x00000784u, 0x00cc516eu, 0xff2c9639u, 0x005ad5b3u, 0x0013ad0du, 0xff3d4799u,
    0x0063ce75u, 0xffb6f398u, 0xff5138d1u, 0x006e9e1fu, 0xff9186e5u, 0xff5f96a4u, 0x0076a86eu,
    0xff82089cu, 0xff676b81u, 0x007b9f8au, 0xff7c48a5u, 0xff6a31e7u, 0x007ebb7bu, 0x00000001u,
};

constexpr uint32_t kCoef32000To48000[kSfcCoefDepth] = {
    0x0015a105u, 0x00000292u, 0x00e4320au, 0xff41d2d9u, 0x004911acu, 0x005dd9e3u, 0xff4c7d80u, 0x0052103eu, 0xfff8ebefu,
    0xff5b6fabu, 0x005f0a0du, 0xffc4b414u, 0xff68582cu, 0x006b38e5u, 0xffabb861u, 0xff704becu, 0x0074de52u, 0xffa19f4cu,
    0xff729059u, 0x007c7e90u, 0x00000003u, 0x00005105u, 0x00000292u, 0x00e4320au, 0xff41d2d9u, 0x004911acu, 0x005dd9e3u,
    0xff4c7d80u, 0x0052103eu, 0xfff8ebefu, 0xff5b6fabu, 0x005f0a0du, 0xffc4b414u, 0xff68582cu, 0x006b38e5u, 0xffabb861u,
    0xff704becu, 0x0074de52u, 0xffa19f4cu, 0xff729059u, 0x007c7e90u, 0x00000001u,
};

constexpr uint32_t kCoef48000To32000[kSfcCoefDepth] = {
    0x00156105u, 0x00000292u, 0x00e4320au, 0xff41d2d9u, 0x004911acu, 0x005dd9e3u, 0xff4c7d80u, 0x0052103eu, 0xfff8ebefu,
    0xff5b6fabu, 0x005f0a0du, 0xffc4b414u, 0xff68582cu, 0x006b38e5u, 0xffabb861u, 0xff704becu, 0x0074de52u, 0xffa19f4cu,
    0xff729059u, 0x007c7e90u, 0x00000002u, 0x00009105u, 0x00000292u, 0x00e4320au, 0xff41d2d9u, 0x004911acu, 0x005dd9e3u,
    0xff4c7d80u, 0x0052103eu, 0xfff8ebefu, 0xff5b6fabu, 0x005f0a0du, 0xffc4b414u, 0xff68582cu, 0x006b38e5u, 0xffabb861u,
    0xff704becu, 0x0074de52u, 0xffa19f4cu, 0xff729059u, 0x007c7e90u, 0x00000001u,
};

constexpr uint32_t kCoef8000To48000[kSfcCoefDepth] = {
    0x00156105u, 0x0000d649u, 0x00e87afbu, 0xff5f69d0u, 0x003df3cfu, 0x007ce488u, 0xff99a5c8u, 0x0056a6a0u, 0x00344928u,
    0xffcba3e5u, 0x006be470u, 0x00137aa7u, 0xffe60276u, 0x00773410u, 0x0005fa2au, 0xfff1ac11u, 0x007c795bu, 0x00012d36u,
    0xfff5eca2u, 0x007f10efu, 0x00000002u, 0x0000a102u, 0x00000e00u, 0x00e2e000u, 0xff6e1a00u, 0x002aaa00u, 0x00610a00u,
    0xff5dda00u, 0x003ccc00u, 0x00163a00u, 0xff3c0400u, 0x00633200u, 0x00000003u,
};

constexpr uint32_t kCoef48000To8000[kSfcCoefDepth] = {
    0x000c9102u, 0x00000e00u, 0x00e2e000u, 0xff6e1a00u, 0x002aaa00u, 0x00610a00u, 0xff5dda00u, 0x003ccc00u, 0x00163a00u,
    0xff3c0400u, 0x00633200u, 0x00000001u, 0x00005105u, 0x0000d649u, 0x00e87afbu, 0xff5f69d0u, 0x003df3cfu, 0x007ce488u,
    0xff99a5c8u, 0x0056a6a0u, 0x00344928u, 0xffcba3e5u, 0x006be470u, 0x00137aa7u, 0xffe60276u, 0x00773410u, 0x0005fa2au,
    0xfff1ac11u, 0x007c795bu, 0x00012d36u, 0xfff5eca2u, 0x007f10efu, 0x00000001u,
};

constexpr uint32_t kCoef22050To48000[kSfcCoefDepth] = {
    0x000c6102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu, 0x001c564cu, 0x00e55557u, 0xffcadd5bu, 0x003d80bau, 0x00d13397u,
    0xfff232f8u, 0x00683337u, 0x00000002u, 0x00186102u, 0x000013d9u, 0x00ebd477u, 0xff4ce383u, 0x0042049du, 0x0089c278u,
    0xff54414du, 0x00531dedu, 0x004a5e07u, 0xff53cf41u, 0x006efbdcu, 0x00000002u, 0x00005204u, 0x000aaaabu, 0xffaaaaabu,
    0xfffaaaabu, 0x00555555u, 0xff600000u, 0xfff55555u, 0x00155555u, 0x00055555u, 0xffeaaaabu, 0x00200000u,
};

constexpr uint32_t kCoef48000To22050[kSfcCoefDepth] = {
    0x000f6103u, 0x000001e0u, 0x00de44c0u, 0xff380b7fu, 0x004ffc73u, 0x00494b44u, 0xff3d493au, 0x005908bfu,
    0xffe9a3c8u, 0xff425647u, 0x006745f7u, 0xffc42d61u, 0xff40a6c7u, 0x00776709u, 0x00000002u, 0x001a5204u,
    0x000aaaabu, 0xffaaaaabu, 0xfffaaaabu, 0x00555555u, 0xff600000u, 0xfff55555u, 0x00155555u, 0x00055555u,
    0xffeaaaabu, 0x00200000u, 0x00005102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu, 0x001c564cu, 0x00e55557u,
    0xffcadd5bu, 0x003d80bau, 0x00d13397u, 0xfff232f8u, 0x00683337u, 0x00000001u,
};

constexpr uint32_t kCoef11025To48000[kSfcCoefDepth] = {
    0x000c6102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu, 0x001c564cu, 0x00e55557u, 0xffcadd5bu, 0x003d80bau,
    0x00d13397u, 0xfff232f8u, 0x00683337u, 0x00000002u, 0x00186102u, 0x000013d9u, 0x00ebd477u, 0xff4ce383u,
    0x0042049du, 0x0089c278u, 0xff54414du, 0x00531dedu, 0x004a5e07u, 0xff53cf41u, 0x006efbdcu, 0x00000002u,
    0x00246102u, 0x0000010au, 0x00c93dc4u, 0xff26f5f6u, 0x005d1041u, 0x001002c4u, 0xff245b76u, 0x00666002u,
    0xffc30a45u, 0xff1baecdu, 0x00765921u, 0x00000002u, 0x00005204u, 0x000aaaabu, 0xffaaaaabu, 0xfffaaaabu,
    0x00555555u, 0xff600000u, 0xfff55555u, 0x00155555u, 0x00055555u, 0xffeaaaabu, 0x00200000u,
};

constexpr uint32_t kCoef48000To11025[kSfcCoefDepth] = {
    0x000c6102u, 0x000000afu, 0x00c65663u, 0xff23d2ceu, 0x005f97d6u, 0x00086ad6u, 0xff20ec4fu, 0x00683201u,
    0xffbbbef6u, 0xff184447u, 0x00770963u, 0x00000002u, 0x00175204u, 0x000aaaabu, 0xffaaaaabu, 0xfffaaaabu,
    0x00555555u, 0xff600000u, 0xfff55555u, 0x00155555u, 0x00055555u, 0xffeaaaabu, 0x00200000u, 0x00235102u,
    0x000013d9u, 0x00ebd477u, 0xff4ce383u, 0x0042049du, 0x0089c278u, 0xff54414du, 0x00531dedu, 0x004a5e07u,
    0xff53cf41u, 0x006efbdcu, 0x00000001u, 0x00005102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu, 0x001c564cu,
    0x00e55557u, 0xffcadd5bu, 0x003d80bau, 0x00d13397u, 0xfff232f8u, 0x00683337u, 0x00000001u,
};

constexpr uint32_t kCoef88200To48000[kSfcCoefDepth] = {
    0x000c6102u, 0x00001685u, 0x00f53ae9u, 0xff52f196u, 0x003e3e08u, 0x00b9f857u, 0xff5d8985u, 0x0050070au, 0x008c3e86u,
    0xff6053f0u, 0x006d98efu, 0x00000002u, 0x00175204u, 0x000aaaabu, 0xffaaaaabu, 0xfffaaaabu, 0x00555555u, 0xff600000u,
    0xfff55555u, 0x00155555u, 0x00055555u, 0xffeaaaabu, 0x00200000u, 0x00005102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu,
    0x001c564cu, 0x00e55557u, 0xffcadd5bu, 0x003d80bau, 0x00d13397u, 0xfff232f8u, 0x00683337u, 0x00000001u,
};

constexpr uint32_t kCoef48000To88200[kSfcCoefDepth] = {
    0x000c6102u, 0x0001d727u, 0x00fc2fc7u, 0xff9bb27bu, 0x001c564cu, 0x00e55557u, 0xffcadd5bu, 0x003d80bau,
    0x00d13397u, 0xfff232f8u, 0x00683337u, 0x00000002u, 0x00186102u, 0x000013d9u, 0x00ebd477u, 0xff4ce383u,
    0x0042049du, 0x0089c278u, 0xff54414du, 0x00531dedu, 0x004a5e07u, 0xff53cf41u, 0x006efbdcu, 0x00000002u,
    0x00230204u, 0x000aaaabu, 0xffaaaaabu, 0xfffaaaabu, 0x00555555u, 0xff600000u, 0xfff55555u, 0x00155555u,
    0x00055555u, 0xffeaaaabu, 0x00200000u, 0x00005102u, 0x00001685u, 0x00f53ae9u, 0xff52f196u, 0x003e3e08u,
    0x00b9f857u, 0xff5d8985u, 0x0050070au, 0x008c3e86u, 0xff6053f0u, 0x006d98efu, 0x00000001u,
};

struct RatioEntry {
    uint32_t inPart;
    uint32_t outPart;
    const uint32_t *coef;
};

constexpr RatioEntry kRatioTable[] = {
    {2u, 1u, kCoef2xDec},
    {1u, 2u, kCoef2xInt},
    {4u, 1u, kCoef4xDec},
    {1u, 4u, kCoef4xInt},
    {147u, 160u, kCoef44100To48000},
    {160u, 147u, kCoef48000To44100},
    {1u, 3u, kCoef16000To48000},
    {3u, 1u, kCoef48000To16000},
    {2u, 3u, kCoef32000To48000},
    {3u, 2u, kCoef48000To32000},
    {1u, 6u, kCoef8000To48000},
    {6u, 1u, kCoef48000To8000},
    {147u, 320u, kCoef22050To48000},
    {320u, 147u, kCoef48000To22050},
    {147u, 640u, kCoef11025To48000},
    {640u, 147u, kCoef48000To11025},
    {147u, 80u, kCoef88200To48000},
    {80u, 147u, kCoef48000To88200},
};

}  // namespace

const uint32_t *sfcCoefficients(uint32_t inRate, uint32_t outRate)
{
    if (switch_audio_sfc_rate_index(inRate) < 0 || switch_audio_sfc_rate_index(outRate) < 0) {
        return nullptr;
    }
    if (inRate == outRate) {
        return nullptr;
    }
    const uint64_t g = switch_audio_gcd64(inRate, outRate);
    const uint32_t inPart = static_cast<uint32_t>(inRate / g);
    const uint32_t outPart = static_cast<uint32_t>(outRate / g);
    for (const RatioEntry &entry : kRatioTable) {
        if (entry.inPart == inPart && entry.outPart == outPart) {
            return entry.coef;
        }
    }
    return nullptr;
}

bool sfcSupportsPair(uint32_t inRate, uint32_t outRate)
{
    if (switch_audio_sfc_rate_index(inRate) < 0 || switch_audio_sfc_rate_index(outRate) < 0) {
        return false;
    }
    if (inRate == outRate) {
        return true;
    }
    return sfcCoefficients(inRate, outRate) != nullptr;
}

bool sfcConfigure(volatile uint8_t *ahub, uint32_t base, uint32_t inRate, uint32_t outRate, uint32_t channels)
{
    if (!ahub || channels < 1u || channels > 2u || !sfcSupportsPair(inRate, outRate)) {
        return false;
    }
    switch_mmio_write32(ahub, base + kSfcEnable, 0u);
    if (!waitReg(ahub, base + kSfcStatus, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    switch_mmio_write32(ahub, base + kSfcCoefRam, 0u);
    switch_mmio_write32(ahub, base + kSfcSoftReset, 1u);
    if (!waitReg(ahub, base + kSfcSoftReset, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    const uint32_t cif = switch_audio_ahub_cif(channels);
    switch_mmio_write32(ahub, base + kSfcRxCif, cif);
    switch_mmio_write32(ahub, base + kSfcTxCif, cif);
    switch_mmio_write32(ahub, base + kSfcRxFreq, static_cast<uint32_t>(switch_audio_sfc_rate_index(inRate)));
    switch_mmio_write32(ahub, base + kSfcTxFreq, static_cast<uint32_t>(switch_audio_sfc_rate_index(outRate)));
    if (const uint32_t *const coef = sfcCoefficients(inRate, outRate)) {
        if (!waitReg(ahub, base + kSfcCfgRamCtrl, kRamCtrlReadBusy, 0u, kTimeoutUs)) {
            return false;
        }
        switch_mmio_write32(ahub, base + kSfcCfgRamCtrl, kRamCtrlWriteSeq);
        for (uint32_t i = 0; i < kSfcCoefDepth; ++i) {
            switch_mmio_write32(ahub, base + kSfcCfgRamData, coef[i]);
        }
        switch_mmio_write32(ahub, base + kSfcCoefRam, 1u);
    }
    return true;
}

void sfcEnable(volatile uint8_t *ahub, uint32_t base)
{
    switch_mmio_write32(ahub, base + kSfcEnable, 1u);
}

bool sfcStop(volatile uint8_t *ahub, uint32_t base)
{
    if (!ahub) {
        return false;
    }
    switch_mmio_write32(ahub, base + kSfcEnable, 0u);
    if (!waitReg(ahub, base + kSfcStatus, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    switch_mmio_write32(ahub, base + kSfcCoefRam, 0u);
    switch_mmio_write32(ahub, base + kSfcSoftReset, 1u);
    return waitReg(ahub, base + kSfcSoftReset, 1u, 0u, kTimeoutUs);
}

}  // namespace switch_audio_ahub
