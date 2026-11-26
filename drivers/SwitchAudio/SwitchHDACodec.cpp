// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchHDA.h"
#include <IOKit/IOLib.h>

namespace {
constexpr uint32_t GCAP = 0x00, STATESTS = 0x0e;
constexpr uint32_t kNvidiaT210CodecId = 0x10de0029u;

constexpr uint32_t kVerbSetStreamFormat = 0x200u;
constexpr uint32_t kVerbSetPowerState = 0x705u;
constexpr uint32_t kVerbSetChannelStreamId = 0x706u;
constexpr uint32_t kVerbSetPinWidgetControl = 0x707u;
constexpr uint32_t kVerbSetDigiConvert1 = 0x70du;
constexpr uint32_t kVerbSetDigiConvert2 = 0x70eu;
constexpr uint32_t kVerbSetStripeControl = 0x724u;
constexpr uint32_t kVerbSetChannelCount = 0x72du;
constexpr uint32_t kVerbGetParameter = 0xf00u;
constexpr uint32_t kVerbGetConnectionList = 0xf02u;
constexpr uint32_t kVerbGetPinSense = 0xf09u;

constexpr uint32_t kParamVendorId = 0x00u;
constexpr uint32_t kParamNodeCount = 0x04u;
constexpr uint32_t kParamFunctionType = 0x05u;
constexpr uint32_t kParamAudioWidgetCap = 0x09u;
constexpr uint32_t kParamPcmRates = 0x0au;
constexpr uint32_t kParamStreamFormats = 0x0bu;

constexpr uint32_t kFuncTypeAudio = 0x01u;
constexpr uint32_t kWidgetTypeAudioOut = 0x0u, kWidgetTypePin = 0x4u;
constexpr uint32_t kCapStereo = 0x001u, kCapFormatOverride = 0x010u, kCapStripe = 0x020u, kCapDigital = 0x200u;
constexpr uint32_t kPinSensePresentEldValid = 0xc0000000u;
constexpr uint32_t kFormatPcm = 1u << 0;
constexpr uint32_t kStreamTag1 = 0x10u, kPinOutEnable = 0x40u;
}  // namespace

bool SwitchHDA::discover(uint32_t formats)
{
    const uint32_t present = core(STATESTS) & 0x7fff;
    if (!present)
        return false;
    uint32_t value = 0;
    afg = converter = pin = 0;
    uint32_t converterCaps = 0;
    for (codec = 0; codec < 15; ++codec) {
        if (!(present & (1u << codec)))
            continue;
        if (!verb(0, kVerbGetParameter, kParamVendorId, &value))
            return false;
        if (value != kNvidiaT210CodecId)
            continue;
        if (!verb(0, kVerbGetParameter, kParamNodeCount, &value))
            return false;
        const uint32_t groups = value & 0xff, first = (value >> 16) & 0xff;
        afg = 0;
        for (uint32_t node = first; node < first + groups && node < 256; ++node) {
            if (!verb(node, kVerbGetParameter, kParamFunctionType, &value))
                return false;
            if ((value & 0xff) == kFuncTypeAudio) {
                afg = node;
                break;
            }
        }
        if (!afg || !verb(afg, kVerbSetPowerState, 0) || !verb(afg, kVerbGetParameter, kParamNodeCount, &value))
            return false;
        const uint32_t widgets = value & 0xff, begin = (value >> 16) & 0xff;
        for (uint32_t node = begin; node < begin + widgets && node < 256; ++node) {
            if (!verb(node, kVerbGetParameter, kParamAudioWidgetCap, &value))
                return false;
            if (((value >> 20) & 15) != kWidgetTypePin || !(value & kCapDigital))
                continue;
            if (!verb(node, kVerbGetPinSense, 0, &value))
                return false;
            if ((value & kPinSensePresentEldValid) != kPinSensePresentEldValid)
                continue;
            pin = node;
            break;
        }
        if (pin)
            break;
    }
    if (!pin || !verb(pin, kVerbGetConnectionList, 0, &value))
        return false;
    converter = value & 0x7f;
    if (!converter || !verb(converter, kVerbGetParameter, kParamAudioWidgetCap, &converterCaps) ||
        ((converterCaps >> 20) & 15) != kWidgetTypeAudioOut ||
        (converterCaps & (kCapDigital | kCapStereo)) != (kCapDigital | kCapStereo))
        return false;
    const uint32_t formatNode = converterCaps & kCapFormatOverride ? converter : afg;
    uint32_t rates[3];
    if (!verb(formatNode, kVerbGetParameter, kParamStreamFormats, &value) || !(value & kFormatPcm) ||
        !verb(formatNode, kVerbGetParameter, kParamPcmRates, &value) || !switch_hda_rates(formats, value, rates))
        return false;
    control->sample_bits = switch_hda_sample_bits(formats, value);
    if (!control->sample_bits)
        return false;
    for (uint32_t i = 0; i < SWITCH_AUDIO_MAX_RATES; ++i)
        control->rates[i] = i < 3 ? rates[i] : 0;
    if (control->rate != rates[0] && control->rate != rates[1])
        setClock(rates[0]);
    const uint32_t gcap = core(GCAP) & 0xffff;
    stripe = converterCaps & kCapStripe ? switch_hda_stripe(gcap, control->sample_bits) : 0;
    if (!programFormat(switch_hda_stream_format(control->sample_bits, control->rate)) ||
        !verb(converter, kVerbSetChannelStreamId, kStreamTag1) || !verb(converter, kVerbSetDigiConvert1, 1) ||
        !verb(converter, kVerbSetDigiConvert2, 0) || !verb(converter, kVerbSetChannelCount, 1) ||
        ((converterCaps & kCapStripe) && !verb(converter, kVerbSetStripeControl, stripe)) ||
        !verb(pin, kVerbSetPinWidgetControl, kPinOutEnable))
        return false;
#if DEVELOPMENT || DEBUG
    IOLog("SwitchAudio: HDA display codec %u: AFG %u, pin %u, converter %u (caps %#x), GCAP %#x, stripe %u, %u-bit, "
          "%u/%u Hz\n",
          codec, afg, pin, converter, converterCaps, gcap, stripe, control->sample_bits, rates[0], rates[1]);
#endif
    return attached;
}

bool SwitchHDA::programFormat(uint16_t format)
{
    return verb(converter, kVerbSetStreamFormat, format);
}
