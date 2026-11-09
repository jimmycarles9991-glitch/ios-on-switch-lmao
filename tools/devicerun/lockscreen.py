"""The iOS 27 lock screen's checks: its GPU work stays on raw, its posters make their scenes, and the time stays clear.

Boot 15 (11-08) is the baseline: posters killed by FrontBoard's 10 s scene-create watchdog while their first command
buffers waited on m2v, backboardd aborted twice on an m2v command buffer that never scheduled, and in landscape the
Celosia (Mercury) poster's floating petals covered the left half of the time.
"""

from tools.devicerun.rules import REPORT, Check

OWNER = 'r117-lockscreen'
REFUSAL = 'com.brunostjohn.switch.refusal'
LOCK_PROCESSES = r'SpringBoard|backboardd|PosterBoard|\w*Poster\w*'
POSTERKIT = 'PosterKit (dyld cache)'

LOCKSCREEN = [
    Check(
        'lockscreen.m2v',
        OWNER,
        'cb.m2v',
        subsystem=REFUSAL,
        regex=rf'^(?P<who>{LOCK_PROCESSES})\[\d+\] cb\.m2v: .*\[first predicate (?P<pred>[\w.]+)\]',
        show=('who', 'pred'),
        source='drivers/TegraGM20BMetal/raw/runtime/RawMetalCommandBuffer.mm',
        note='first m2v command buffer per process and reason; boot 15: enc.ubo, enc.notic, pass.fmt, blit.b2t, '
        'tex.shape, compute',
    ),
    Check(
        'lockscreen.scene-watchdog',
        OWNER,
        'Provision violated for watchdog scene-create',
        regex=r'\[extension<(?P<extension>[\w.]+):\d+\] Provision violated',
        show=('process', 'extension'),
        source='FrontBoard (dyld cache)',
        note='a poster extension killed 0x8badf00d before its scene was made; boot 15: Aegir, Emoji, Mercury, '
        'Kaleidoscope, Gradient',
    ),
    Check(
        'lockscreen.display-abort',
        OWNER,
        'not done after',
        process='backboardd',
        subsystem='com.brunostjohn.switch.TegraGM20BMetal',
        regex=r'raw: (?P<wait>.+?),? (?:of )?command buffer 0x[0-9a-f]+ not done',
        show=('wait',),
        source='drivers/TegraGM20BMetal/raw/runtime/RawMetal.mm',
        note='the window server aborted on a command buffer: boot 15 twice (19:20:00, 19:21:36), m2v pending 1 '
        'unscheduled 1; RawMetalSubmit.mm aborts a raw command buffer held behind m2v work the same way',
    ),
    Check(
        'lockscreen.hero-fits-time',
        OWNER,
        'currentTallest: ',
        REPORT,
        process='MercuryPosterExtension',
        regex=r'currentTallest: (?P<tallest>[\d.]+), upper: (?P<upper>[\d.]+), lower: (?P<lower>[\d.]+)',
        show=('tallest', 'upper'),
        source='MercuryPosterExtension (Celosia, an error line)',
        note="Celosia logs it when its own time allowance (0.6 of the canvas, its gallery title style's "
        'preferredTimeMaxY in both orientations) is past what its flower fits at this aspect (16:9 landscape: '
        '0.44). The time SpringBoard draws is compact (lockscreen.adaptive-time) and ends above that',
    ),
    Check(
        'lockscreen.adaptive-time',
        OWNER,
        '[Adaptive Time] Calculated timeHeight',
        REPORT,
        process='SpringBoard',
        regex=r'timeHeight: (?P<height>[\d.]+) from honorsSalientContent: (?P<honors>\w+).*'
        r'salientSpaceLeft: (?P<space>-?[\d.]+)',
        show=('height', 'honors', 'space'),
        source='CoverSheet (dyld cache)',
        note='the time SpringBoard draws; boot 15: 132.8 pt, honorsSalientContent false, salientSpaceLeft -96.8',
    ),
    Check(
        'lockscreen.host-rotation',
        OWNER,
        'MaglevDynamicOrientationUpdate',
        REPORT,
        process='SpringBoard',
        source='PaperBoardUI (dyld cache)',
        note='the lock screen host sends posters only the device orientation (dynamic rotation)',
    ),
    Check(
        'lockscreen.poster-orientation',
        OWNER,
        'Device Orientation has updated',
        REPORT,
        regex=r'from (?P<previous>\S+) -> UIInterfaceOrientation(?P<to>\w+)',
        show=('process', 'to'),
        source=POSTERKIT,
        note='what each poster renderer was told; read with the snapshot check in the HW Test sidecar',
    ),
]
