// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

// "split twins=<n> whole=<why>:<n>,...": draws of states with a split since the last line, those that took their twin and
// those that did not by why (raw_buffer_writes.h SplitWhole); nothing where there were none
size_t raw_metal_split_rates(char *out, size_t size)
{
#if SWITCH_RAW_DIAGNOSTICS
    if (!gRM || !size)
        return 0;
    static const char *whys[] = {"forced", "box", "unbound", "written", "unmade"};
    static_assert(std::size(whys) == size_t(raw::SplitWhole::Count));
    raw::Device::Stats &k = gRM->raw->stats;
    uint64_t whole[std::size(whys)], all = 0;
    for (size_t w = 0; w < std::size(whys); w++)
        all += whole[w] = k.splitWhole[w].exchange(0);
    const uint64_t twins = k.splitTwins.exchange(0);
    if (!twins && !all)
        return 0;
    int at = snprintf(out, size, "split twins=%llu whole=", (unsigned long long)twins);
    bool any = false;
    for (size_t w = 0; w < std::size(whys); w++)
        if (whole[w] && at > 0 && size_t(at) < size) {
            at += snprintf(out + at, size - size_t(at), "%s%s:%llu", any ? "," : "", whys[w],
                           (unsigned long long)whole[w]);
            any = true;
        }
    if (!any && at > 0 && size_t(at) < size)
        at += snprintf(out + at, size - size_t(at), "-");
    return at > 0 ? std::min(size_t(at), size - 1) : 0;
#else
    (void)out, (void)size;
    return 0;
#endif
}
