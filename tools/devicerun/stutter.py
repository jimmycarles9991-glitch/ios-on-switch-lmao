"""The frame-pacing checks: one line per stutter source, and the number that means it is gone.

Every line here is debug-kernel only (SWITCH_FRAME_TIMELINE, the power stats line), so a release boot reads NO-DATA.
Pull right after the morning choreography (morning.py CHECKLIST), so the windows hold swipes, scrolls and launches.
The same lines come from both kinds (17 and 27).
"""

from tools.devicerun.rules import REPORT, Check

KERNEL = 'kernel'
OWNER = 'r73-stutter'
STATS = 'SwitchDisplay stats: '
PACING = 'SwitchDisplay pacing: '
POWER = 'SwitchPower: '
RENDER = 'SwitchFrameRender: vsync'
DISPLAY = 'drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp'
IDLE = 'drivers/SwitchPower/SwitchPowerStats.cpp, kernel/switch/idle/idle.c'
RAW = 'com.brunostjohn.switch.TegraGM20BMetal'
RATES = 'drivers/TegraGM20BMetal/raw/runtime/RawMetalRates.mm'
RENDER_SERVER = 'backboardd'
LONG = (
    r'long \d+: idle (?P<idle>\d+)/\d+ ca (?P<ca>\d+)/\d+ gpu (?P<gpu>\d+)/\d+ queue (?P<queue>\d+)/\d+ '
    r'latch (?P<latch>\d+)/\d+ rate (?P<rate>\d+)/\d+'
)
VSYNC = r'vsync folded (?P<folded>\d+) late>1ms (?P<late>\d+)'
C7_EXITS = r'c7 timer exits (?P<exits>\d+), mean (?P<mean>\d+) us, boot max (?P<max>\d+) us, (?P<late>\d+) late'

STUTTER = [
    Check(
        'stutter.vsync-delivered',
        OWNER,
        STATS,
        'min(sent/vblanks) >= 0.98',
        process=KERNEL,
        regex=r'vsync (?P<sent>\d+) sent [\d.]+/s of (?P<vblanks>\d+) vblanks',
        where='vblanks >= 30',
        source=DISPLAY,
        note='a vsync client paced by DC A V_BLANK. 11-06: 298 of 299, when the 21H461 kernel forced CA onto vsync with '
        "CA_FORCE_TIMER=1. Neither kernel does now: a J120's and a J181's CA run a CADisplayTimer phased by frame info "
        '(kern_exec.c), so this reads NO-DATA unless another client asks for vsync',
    ),
    Check(
        'stutter.vsync-late',
        OWNER,
        STATS,
        'max(max) <= 1000',
        process=KERNEL,
        regex=r'vblanks, late (?P<mean>\d+)/(?P<max>\d+) us',
        source=DISPLAY,
        note='V_BLANK to the render server notification, us; 11-06 max 2946',
    ),
    Check(
        'stutter.vsync-late-1ms',
        OWNER,
        PACING,
        'sum(late) == 0',
        process=KERNEL,
        regex=VSYNC,
        source=DISPLAY,
    ),
    Check(
        'stutter.vsync-folded',
        OWNER,
        PACING,
        'sum(folded) == 0',
        process=KERNEL,
        regex=VSYNC,
        source=DISPLAY,
        note='two V_BLANKs before the work loop ran: a vsync the render server never got',
    ),
    Check(
        'stutter.frame-end-late',
        OWNER,
        STATS,
        'max(max) <= 1000',
        process=KERNEL,
        regex=r'fe-late (?P<mean>\d+)/(?P<max>\d+) us',
        source=DISPLAY,
        note='frame-end filter to its work-loop action (flip checks), us; 11-06 max 11368',
    ),
    Check('stutter.long-ca', OWNER, PACING, 'sum(ca) == 0', process=KERNEL, regex=LONG, source=DISPLAY),
    Check(
        'stutter.long-gpu',
        OWNER,
        PACING,
        'sum(gpu) == 0',
        process=KERNEL,
        regex=LONG,
        source=DISPLAY,
        note='the GPU ran past the boundary; frames.slow-presents splits them',
    ),
    Check('stutter.long-queue', OWNER, PACING, 'sum(queue) == 0', process=KERNEL, regex=LONG, source=DISPLAY),
    Check('stutter.long-latch', OWNER, PACING, 'sum(latch) == 0', process=KERNEL, regex=LONG, source=DISPLAY),
    Check(
        'stutter.c7-frame-wakes',
        OWNER,
        POWER,
        'max(wakes) <= 10',
        process=KERNEL,
        regex=r'cpu3 c7 [\d.]+% \(\d+, \d+ skipped, (?P<wakes>\d+) frame wakes\)',
        source=IDLE,
        note='frame lines (all on CPU3) that found it in C7, per 10 s: one per animation start after 100 ms still is '
        'the design (Control Center x5 is five); tens mean the frame core power-gates between frames',
    ),
    Check(
        'stutter.c7-exit-max',
        OWNER,
        POWER,
        'max(max) <= 130',
        process=KERNEL,
        regex=C7_EXITS,
        source=IDLE,
        note='C7 exit measured from the wake timer, us; the governor budgets the console DT wakeup-latency, 130',
    ),
    Check('stutter.c7-exit-late', OWNER, POWER, 'sum(late) == 0', process=KERNEL, regex=C7_EXITS, source=IDLE),
    Check(
        'stutter.emc-raise',
        OWNER,
        POWER,
        'max(max) <= 2000',
        process=KERNEL,
        regex=r'(?P<raises>\d+) raises: hint to top (?P<mean>\d+)/(?P<max>\d+) us \(BPMP max (?P<bpmp>\d+) us\)',
        source='drivers/SwitchPower/SwitchEMC.cpp',
        note='touch, first frame line or GPU submit to the EMC at its top rate, us: slower than the first frame '
        "needs it leaves that frame's GPU work on a slow EMC",
    ),
    Check(
        'stutter.render-core-wait',
        OWNER,
        RENDER,
        'max(runnable) <= 1000',
        process=KERNEL,
        regex=r'waiting for a core (?P<runnable>\d+) us',
        source='kernel/switch/sched/render.c',
        note="CA's display thread runnable but off a core, per slow frame, us: preemption or a C7 wake",
    ),
    Check(
        'stutter.render-blocks',
        OWNER,
        RENDER,
        REPORT,
        process=KERNEL,
        regex=r'longest block (?P<longest>\d+) us, user pc (?P<pc>\S+) lr \S+, '
        r'kernel continuation (?P<kernel>0x[0-9a-f]+)',
        show=('kernel', 'pc'),
        source='kernel/switch/sched/render.c',
        note="where CA's display thread waited in slow frames (ipsw dyld a2s the pc)",
    ),
    Check(
        'stutter.cpu-at-vsync',
        OWNER,
        'SwitchFrame: latch',
        'min(vsync) >= 1683',
        process=KERNEL,
        regex=r'cpu (?P<vsync>\d+) MHz at vsync (?P<swap>\d+) at swap',
        source=DISPLAY,
        note='CPU clock at the vsync of each slow frame: touch, missed-frame and launch hints boost it to the '
        'ceiling, 1785 or 1683 MHz where the DFLL row measured high',
    ),
    Check(
        'stutter.gpu-raise',
        OWNER,
        'tegra-gm20b: idle clock ',
        'max(max) <= 100000',
        process=KERNEL,
        regex=r'raise mean (?P<mean>\d+) ns max (?P<max>\d+) ns',
        source='drivers/TegraGM20B/gpu_inline.h idle_stats',
        note='76.8 -> 768 MHz before the kick that needs it, ns: a PL change and two 2 us delays',
    ),
    Check(
        'stutter.raw-scheduled-wait',
        OWNER,
        'raw ',
        'max(us) <= 5000',
        process=RENDER_SERVER,
        subsystem=RAW,
        regex=r' ws=(?P<waits>\d+)/(?P<us>\d+)us',
        source=RATES,
        note="the render thread blocked in waitUntilScheduled, us per 5 s: a scheduled-handler hop or m2v's scheduling",
    ),
    Check(
        'stutter.raw-completed-wait',
        OWNER,
        'raw ',
        'sum(waits) == 0',
        process=RENDER_SERVER,
        subsystem=RAW,
        regex=r' wc=(?P<waits>\d+)/(?P<us>\d+)us',
        source=RATES,
        note='the render server waited for the GPU to finish a command buffer',
    ),
    Check(
        'stutter.raw-cpu-ordered',
        OWNER,
        'raw ',
        'sum(cpu) == 0',
        subsystem=RAW,
        regex=r' sw=\d+ cpu=(?P<cpu>\d+) ',
        source=RATES,
        note='raw and m2v ordered by a CPU wait instead of the GPU',
    ),
    Check(
        'stutter.raw-ring-waits',
        OWNER,
        'raw ',
        'sum(rw) == 0',
        subsystem=RAW,
        regex=r' rw=(?P<rw>\d+) pool=',
        source=RATES,
        note='the encoder waited for ring space',
    ),
    Check(
        'stutter.raw-pool-stalls',
        OWNER,
        'raw ',
        'sum(grow) == 0',
        subsystem=RAW,
        regex=r' grow=(?P<grow>\d+) ',
        source=RATES,
        note='an allocation waited for the pool to grow',
    ),
]
