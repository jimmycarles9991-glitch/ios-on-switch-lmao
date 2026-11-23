// SPDX-License-Identifier: MIT
// DEVELOPMENT: how a frame-weights capture (draw_times.h) is asked for. From the Mac, for one named process:
// `idevicenotificationproxy -u <udid> post com.brunostjohn.switch.debug.frame-weights.<process>` (backboardd: the
// compositor) captures that process's next kTimedAskedByName command buffers, whose "SwitchMetal: fw" lines
// tools/devicerun/frame_weights.py reads from the pulled log archive. HW Test asks its own process for an exact count and
// reads the marks back (switchMetalFrameWeights:, switchMetalFrameWeightsTaken).
#import "TegraGM20BMetalInternal.h"

#include <notify.h>
#include <stdlib.h>
#include "draw_capture.h"

#if SWITCH_METAL_DIAGNOSTICS
void watchFrameWeights(gm20b::Device *dev)
{
    char name[192];
    snprintf(name, sizeof name, "com.brunostjohn.switch.debug.frame-weights.%s", getprogname());
    int token;
    notify_register_dispatch(name, &token, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
        dev->askDrawTimes(gm20b::kTimedAskedByName);
        os_log(gm20bLog(), "SwitchMetal: fw asked for %u command buffers (debug notification)",
               gm20b::kTimedAskedByName);
    });
}

static NSDictionary *takenRow(const gm20b::DrawTimesTaken::Row &r)
{
    const gm20b::TimedLabel &l = r.row.label;
    const bool drawn = l.kind == gm20b::TimedKind::Draw || l.kind == gm20b::TimedKind::Indirect;
    return @{
        @"buffer" : @(r.buffer),
        @"kind" : [NSString stringWithFormat:@"%c", gm20b::timedKindLetter(l.kind)],
        @"key" : drawn ? [NSString stringWithFormat:@"%016llx", (unsigned long long)r.state.key] : @"",
        @"name" : r.state.name ? @(r.state.name) : @"",
        @"form" : drawn ? @(gm20b::spanFormName(gm20b::SpanForm(r.state.form))) : @"",
        @"fragment_words" : @(r.state.fragmentWords),
        @"gprs" : @(r.state.gprs),
        @"pass" : l.pass == gm20b::kNoPass ? @(-1) : @(l.pass),
        @"format" : @(r.pass.format),
        @"width" : @(r.pass.width),
        @"height" : @(r.pass.height),
        @"topology" : @(l.topology),
        @"vertices" : @(l.vertices),
        @"instances" : @(l.instances),
        @"box" : @(l.box),
        @"invocations" : @(r.row.invocations),
        @"ns" : @(r.row.ns),
    };
}

@implementation SwitchMetalDevice (FrameWeights)

// The next `buffers` command buffers this process begins are captured: nil, else why not
- (NSString *)switchMetalFrameWeights:(NSNumber *)buffers
{
    const unsigned n = buffers.unsignedIntValue;
    if (!n || n > gm20b::kTimedBuffers)
        return [NSString stringWithFormat:@"a capture takes 1 to %u command buffers", gm20b::kTimedBuffers];
    gRM->raw->askDrawTimes(n);
    return nil;
}

// The capture's marks read back so far, once its command buffers completed: capture, ended, reference_ns (each command
// buffer's GPU time before it, -1 unknown), buffers and rows in GPU order
- (NSDictionary *)switchMetalFrameWeightsTaken
{
    const gm20b::DrawTimesTaken t = gRM->raw->drawTimesTaken();
    NSMutableArray *reference = [NSMutableArray array];
    for (int64_t ns : t.reference)
        [reference addObject:@(ns)];
    NSMutableArray *buffers = [NSMutableArray array];
    for (const gm20b::DrawTimesTaken::Buffer &b : t.buffers)
        [buffers addObject:@{
            @"buffer" : @(b.index),
            @"stamp" : @(b.stamp),
            @"gpu_ns" : @(b.gpuNs),
            @"marks" : @(b.marks),
            @"unwritten" : @(b.unwritten),
            @"full" : @(b.full),
        }];
    NSMutableArray *rows = [NSMutableArray array];
    for (const gm20b::DrawTimesTaken::Row &r : t.rows)
        [rows addObject:takenRow(r)];
    return @{
        @"capture" : @(t.serial),
        @"ended" : @(t.ended),
        @"reference_ns" : reference,
        @"buffers" : buffers,
        @"rows" : rows,
    };
}

@end
#endif
