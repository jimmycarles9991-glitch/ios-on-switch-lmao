"""r30-frames' morning GPU and frame run: its log-checkable items and the manual steps printed before the table.

Boot A is the plain debug build, B adds --//drivers/TegraGM20B:tiled_cache, K adds --//kernel:kprof. The source of
both lists is out/agent-scratch/r30-frames/morning/morning-doc.txt; its appendices hold the readouts that need
tools/frames:gpucapture rather than a count.
"""

from tools.devicerun.rules import REPORT, TIMELINE, Check

KERNEL = 'kernel'
RAW = 'com.brunostjohn.switch.TegraGM20BMetal'
DOC = 'r30-frames morning-doc.txt'
PENDING = 'not on deploy-next5 (r30-frames morning-doc.txt)'
RAW_5S = r'raw \d+s c=(?P<c>\d+)\(\S+\) m2v=(?P<m2v>\d+)(?:\((?P<reasons>[^)]*)\))?'

MORNING = [
    Check(
        'raw.m2v',
        'r31-rawrt',
        'raw ',
        'p50(m2v) == 0',
        subsystem=RAW,
        regex=RAW_5S,
        source='drivers/TegraGM20BMetal/raw/runtime/RawMetalRates.mm',
        note='m2v=0 in steady App Library scroll',
    ),
    Check('raw.m2v-xr', 'r31-rawrt', 'raw ', subsystem=RAW, regex=r'm2v=\d+\([^)]*XR', source=DOC),
    Check('raw.no-m2v-twin', 'r30-alias', 'no m2v twin', source=DOC),
    Check('alias.nocopy-hostmemory', 'r30-alias', 'no-copy', regex='HostMemory', source=DOC),
    Check(
        'gpu.tiled-cache-on',
        'r31-gpuanatomy',
        'raw: tiled cache on',
        'count > 0',
        subsystem=RAW,
        boot='B',
        source=PENDING,
    ),
    Check('gpu.capture-truncated', 'r31-gpuanatomy', 'SwitchFrameGpu:', regex='truncated', source=PENDING),
    Check(
        'planes.dropped',
        'r31-cadeadline',
        'SwitchDisplay pacing: ',
        'sum(dropped) == 0',
        process=KERNEL,
        regex=r'dropped (?P<dropped>\d+), underflow (?P<underflow>\d+), unready (?P<unready>\d+)',
        boot='A',
        source=DOC,
        note='video step',
    ),
    Check(
        'planes.underflow',
        'r31-cadeadline',
        'SwitchDisplay pacing: ',
        'sum(underflow) == 0',
        process=KERNEL,
        regex=r'underflow (?P<underflow>\d+)',
        boot='A',
        source=DOC,
    ),
    Check(
        'planes.unready',
        'r31-cadeadline',
        'SwitchDisplay pacing: ',
        'sum(unready) == 0',
        process=KERNEL,
        regex=r'unready (?P<unready>\d+)',
        boot='A',
        source=DOC,
    ),
    Check(
        'gpu.rf16-rounding',
        'r31-rawds',
        ('rf16', 'RF16'),
        REPORT,
        boot='A',
        regex=r'(?P<halves>[0-9a-f]{4} [0-9a-f]{4} [0-9a-f]{4} [0-9a-f]{4})',
        show=('halves',),
        source=PENDING,
        note='RNE reads 3800 3800 37ff 3800',
    ),
    Check(
        'widget.needs-display',
        'r31-rawds',
        'setNeedsDisplay caller',
        REPORT,
        boot='A',
        show=('process',),
        source=PENDING,
        note='Clock widget A/B: 20 Hz must stop when the widget is removed',
    ),
    Check(
        'widget.display-link', 'r31-rawds', 'displayLinkWithTarget', REPORT, boot='A', show=('process',), source=PENDING
    ),
    Check('nvk.copy-ways', 'r32-gpufixed', 'nvk: copies and fills on', TIMELINE, boot='A', source=DOC),
    Check('frames.render-thread', 'r31-cadeadline', 'SwitchFrameRender: vsync', REPORT, process=KERNEL, source=PENDING),
]

CHECKLIST = {
    '': [
        'Before: open Settings, Safari and Photos once; a Clock widget on Home page 1; note the wall-clock time.',
        '1 Home, hands off, 60 s   2 Home pages + widgets, 20 s   3 App Library scroll, 30 s',
        '4 Spotlight typing, 20 s   5 App switcher, 20 s   6 Control Center x5, then Notification Center x5',
        '7 Home folder open/close x5   8 App Library scroll again, 15 s',
        (
            'Then the same steps on the iOS 27 image, pulled separately: its stutter.* rows hold too '
            "(stutter.vsync-delivered reads NO-DATA on both: Core Animation's frame clock follows frame info)"
        ),
    ],
    'A': [
        '9 Full-screen video in Tips or Photos: 15 s controls hidden, 5 s showing',
        '10 Scroll App Library; post fetch-barrier-psonly, -none, -ps 10 s apart (idevicenotificationproxy)',
        '11 With a widget on screen: post com.brunostjohn.switch.debug.rf16-rounding',
        '12 Clock widget on Home 30 s hands off, then removed 30 s',
        '13 NVK copies meta vs copy engine (Appendix 3)   14 WebGL hog in Safari, 20 s (if r32-gpudeadline landed)',
        '15 Home, plugged in, 5 min hands off, then open the switcher once',
    ],
    'B': ['Compare with A: bazel run //tools/frames:gpucapture -- <A>/a.logarchive --compare <B>/a.logarchive'],
    'K': ['kprof boot: the final deploy has no kprof; remind the user'],
}
