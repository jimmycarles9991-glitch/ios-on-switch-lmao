"""What the m2v cut (no Metal-to-Vulkan device, no NVK, no replay, no twins) leaves in the device log of every process.

Lines of code the cut deleted are an old plugin when they appear. A private selector no Metal protocol declares, which
only the old front end's classes answered, now raises in the process that sends it: that crash names the selector
here and in the crash reports (reports.py crash.gm20b-selector).
"""

from tools.devicerun.rules import REPORT, Check

REFUSAL_SUBSYSTEM = 'com.brunostjohn.switch.refusal'
GPU_METAL_SUBSYSTEM = 'com.brunostjohn.switch.TegraGM20BMetal'
PLUGIN = 'drivers/TegraGM20BMetal/TegraGM20BMetalPlugin.mm'
SUBMIT = 'drivers/TegraGM20BMetal/TegraGM20BMetalSubmit.mm'
OLD_PLUGIN = 'the string is gone with the cut: any line is an old plugin'
# A refusal line: `<process>[<pid>] <reason>: <message>` (kits/SwitchRefusal/SwitchRefusal.c)
REFUSAL = r'^(?P<who>[^\[]+)\[\d+\] (?P<reason>{reasons}): '
# The debug rates line (TegraGM20BMetalRates.mm): `raw <seconds>s c=<command buffers>(...`
RATES = r'\bgm20b \d+s c=\d+\('
# A process TEGRA_GPU_ALL_CLIENTS=0 leaves out on purpose
LEFT_OUT = r'(?!.*left out \(TEGRA_GPU_ALL_CLIENTS=0\)$)'

CUT = [
    Check(
        'cut.m2v-opener',
        'm2v-cut1',
        ('needs its m2v device for', 'opened its m2v device for'),
        source='GM20BMetalM2V.mm (deleted)',
        note=OLD_PLUGIN,
    ),
    Check(
        'cut.m2v-ready-line',
        'm2v-cut1',
        '(m2v device at its first need)',
        source=PLUGIN,
        note="the old plugin's ready line: the card holds a plugin from before the cut",
    ),
    Check(
        'cut.encode-stopped',
        'm2v-cut1',
        "a range's encode stopped with no command buffer error",
        regex=r'encode stopped with no command buffer error \((?P<why>[^)]*)\)',
        show=('process', 'why'),
        source=SUBMIT,
        note='a defect: its why names the step',
    ),
    Check(
        'cut.no-device',
        'm2v-cut1',
        'TegraGM20BMetal: no device in ',
        regex=r'^TegraGM20BMetal: no device in (?P<who>\S+) \((?P<why>[^)]*)\)' + LEFT_OUT,
        show=('who', 'why'),
        source=PLUGIN,
        note='every Metal process opens its device; a process left out by TEGRA_GPU_ALL_CLIENTS=0 is not counted. '
        "'no AGXAccelerator' or 'IOServiceOpen failed' here: the service match the device makes itself",
    ),
    Check('cut.path-off', 'm2v-cut1', 'raw GM20B path off', source=PLUGIN, note=OLD_PLUGIN),
    Check(
        'cut.frontend-line',
        'm2v-cut4',
        'native-metal: ',
        show=('process',),
        source='the old Metal front end (no longer built, out of the image)',
        note='only the old front end logged these: any line is a front end loaded again',
    ),
    Check('cut.m2v-device-line', 'm2v-cut1', 'TegraGM20BMetal: m2v device', source=PLUGIN, note=OLD_PLUGIN),
    Check(
        'cut.path-on',
        'm2v-cut1',
        'TegraGM20BMetal: raw GM20B path on in ',
        REPORT,
        regex=r'path on in (?P<who>\S+) \(',
        key='who',
        show=('who',),
        source=PLUGIN,
        note='to see at least once: backboardd, SpringBoard, iconservicesagent, ScreenshotServicesService, '
        'com.apple.WebKit.GPU, WidgetRenderer_*, com.apple.MapKit.SnapshotService, a PosterBoard extension, '
        'SwitchHWTest',
    ),
    Check(
        'cut.device-ready',
        'm2v-cut1',
        'TegraGM20BMetal: device ',
        REPORT,
        regex=r' ready in (?P<who>[^,]+), open \d+ ms',
        key='who',
        against='cut.path-on',
        show=('who',),
        source=PLUGIN,
        note='every process whose device is ready logged the driver on before it',
    ),
    Check(
        'cut.record-refusals',
        'm2v-cut2',
        ('call.buf: ', 'state.foreign: ', 'tex.foreign: '),
        REPORT,
        subsystem=REFUSAL_SUBSYSTEM,
        regex=REFUSAL.format(reasons=r'call\.buf|state\.foreign|tex\.foreign'),
        show=('who', 'reason'),
        source='drivers/TegraGM20BMetal (gm20bCallInvalid)',
        note="a client's object of another device, failed as Metal's validation error: report the process",
    ),
    Check(
        'cut.state-pack-gone',
        'm2v-cut3',
        ('state.notpack: ', 'state.pack: ', 'state.unpacked: '),
        subsystem=REFUSAL_SUBSYSTEM,
        regex=REFUSAL.format(reasons=r'state\.notpack|state\.pack|state\.unpacked'),
        show=('who', 'reason'),
        note=OLD_PLUGIN,
    ),
    Check(
        'cut.ended-encoder',
        'm2v-cut2',
        'plan.ended_encoder: ',
        process='!SwitchHWTest',
        subsystem=REFUSAL_SUBSYSTEM,
        regex=REFUSAL.format(reasons=r'plan\.ended_encoder'),
        show=('who',),
        source=SUBMIT,
        note="a client encoded on an encoder that had ended: report the process (HW Test's gpu.gm20b.ended_encoder "
        'does so on purpose)',
    ),
    Check('cut.order-refused', 'm2v-cut2', 'the kernel refused Order', note=OLD_PLUGIN),
    Check(
        'cut.kernel-legacy-line',
        'm2v-cut5',
        ('address_jobs=', 'SubmitAddress', 'GPU hang detail:', 'order wait'),
        process='kernel',
        regex=r'(?P<what>address_jobs=|SubmitAddress|order wait|GPU hang detail:.* waits on)',
        show=('what',),
        source='drivers/TegraGM20B (Order, Exec, ExecSurfaces, Submit and SubmitAddress left the kernel)',
        note="the kernel's paths line lost address_jobs=, its exec-by-process line SubmitAddress, its hang detail "
        "'waits on' and its hang panic and acquire timeouts 'order wait': any is a kernel from before",
    ),
    Check(
        'cut.stall-m2v',
        'm2v-cut2',
        'SwitchMetal: stall:',
        regex=r'm2v (?:pending|unscheduled)',
        note='a stall line naming m2v work: an old plugin',
    ),
    Check(
        'cut.rates',
        'm2v-cut2',
        's c=',
        REPORT,
        subsystem=GPU_METAL_SUBSYSTEM,
        regex=RATES,
        show=('process',),
        source='drivers/TegraGM20BMetal/TegraGM20BMetalRates.mm',
        note='debug images: the rates line still comes',
    ),
    Check(
        'cut.rates-fields',
        'm2v-cut2',
        's c=',
        subsystem=GPU_METAL_SUBSYSTEM,
        regex=RATES + r'.*? (?P<field>m2v|sw|cpu|acq|replay|rclean|why|rsplit|rmax|rdraw|tw)=',
        show=('field',),
        needs='cut.rates',
        source='drivers/TegraGM20BMetal/TegraGM20BMetalRates.mm',
        note='a field the cut removed from the rates line: an old plugin',
    ),
    Check(
        'cut.no-render-state',
        'm2v-cut3',
        ': no render state ',
        REPORT,
        regex=r'^SwitchMetal: (?P<who>[^:]+): no render state ',
        show=('who',),
        source='drivers/TegraGM20BMetal (s4a)',
        note="a render state of another device's functions or of no vertex function, nil: a client defect",
    ),
    Check(
        'cut.deleted-tags',
        'm2v-cut3',
        ('twin.texture: ', 'tex.swizzle: ', 'selector.missing: '),
        subsystem=REFUSAL_SUBSYSTEM,
        regex=REFUSAL.format(reasons=r'twin\.texture|tex\.swizzle|selector\.missing'),
        show=('who', 'reason'),
        note=OLD_PLUGIN,
    ),
    Check('cut.selector-zero-m2v', 'm2v-cut3', "as m2v's object answers it", note=OLD_PLUGIN),
    Check(
        'cut.selector-raised',
        'm2v-cut3',
        'unrecognized selector sent to',
        regex=r'-\[(?P<cls>SwitchMetal\w+) (?P<selector>[^\]]+)\]: unrecognized selector sent to',
        show=('process', 'cls', 'selector'),
        source='libobjc (doesNotRecognizeSelector:)',
        note='a selector no Metal protocol declares that only the old front end answered raises and crashes its '
        'process: the row names it',
    ),
    Check(
        'cut.out-of-va',
        'm2v-cut3',
        'out of GPU VA',
        show=('process',),
        source='drivers/TegraGM20BMetal/kernel.cpp',
        note="a raw device places in its connection's whole VA range: never runs out",
    ),
]
