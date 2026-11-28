"""The device checks owed by the frame, boot, dma, display, power, hwtest2 and measure lanes (owed.py).

Each `source` names the file that logs the line and the branch tip it was read at (25-26 Nov): a lane's line reads
NO-DATA until its branch is on the card.
"""

from tools.devicerun.owed import METAL, Ref, line, none_of, row
from tools.devicerun.rules import REPORT, SUMS, TIMELINE

FRAME = [
    line(
        'frame',
        'early-schedule',
        'TegraGM20BMetal: ',
        SUMS,
        process='backboardd',
        subsystem=METAL,
        regex=(
            r'^TegraGM20BMetal: \S+ frame pass=.*? ws=(?P<waits>\d+)/(?P<waited_us>\d+)us.*? early=(?P<made>\d+)/'
            r'(?P<shape>\d+)/(?P<pending>\d+)/(?P<order>\d+)/(?P<small>\d+)/(?P<busy>\d+)/(?P<kernel>\d+)'
        ),
        show=('made', 'shape', 'pending', 'order', 'small', 'busy', 'kernel', 'waits', 'waited_us'),
        source='drivers/TegraGM20BMetal/TegraGM20BMetalEncoderRates.mm (big/frame d1eac1c1c)',
        note='early=: frames scheduled early, then each refusal (frame_cpu_counts.h EarlyOutcome); ws=: '
        'waitUntilScheduled waits, near 0 for split frames once the early schedule is fixed',
    ),
    line(
        'frame',
        'early-promise-refused',
        'early writer promise',
        REPORT,
        process='backboardd',
        subsystem=METAL,
        regex=(
            r"refused the render server's early writer promise for (?P<surfaces>\d+) surfaces "
            r'\((?P<code>0x[0-9a-f]+)\)'
        ),
        show=('code',),
        source='drivers/TegraGM20BMetal/TegraGM20BMetalSchedule.mm (big/frame d1eac1c1c)',
        note='once a boot when the kernel refuses: its result code',
    ),
    row('frame', 'gpu.sched.long_draw_bands', note='unchanged by big/frame'),
]

SD = 'drivers/NativeSwitchBlock/NativeSwitchBlock.cpp (big/boot d4c468245)'
BOOT = [
    line(
        'boot',
        'sd-reprio',
        'SwitchSD: ',
        'max(xnu_raised) > 0',
        regex=r' reprio xnu (?P<xnu_calls>\d+)/(?P<xnu_raised>\d+) cryptex (?P<cx_calls>\d+)/(?P<cx_raised>\d+)',
        source=SD,
        note="raise calls since boot / queued requests raised: 0 raised means XNU's busy-page raise does not reach "
        'the card (36/0 in the emulator)',
    ),
    line(
        'boot',
        'cryptex-set-priority',
        'SwitchCryptex: ',
        'max(set_priority) > 0',
        regex=r'raises since boot: joined (?P<joined>\d+), doSetPriority (?P<set_priority>\d+), late (?P<late>\d+)',
        source='drivers/NativeSwitchBlock/SwitchCryptexStats.h (big/boot d4c468245)',
        note='0 on the cryptex volumes: APFS does not forward setPriority there and the Default floor stays',
    ),
    line(
        'boot',
        'backboardd-startup',
        'startup complete',
        'min(s) < 20.57',
        process='backboardd',
        regex=r'startup complete\.? (?P<s>[\d.]+) ?s(?:econds)? elapsed',
        note="Apple's line, once at boot: 20.57 s before",
    ),
    line(
        'boot',
        'progressui-screen-class',
        'got screen class',
        'max(screen_class) >= 0',
        process='backboardd',
        regex=r'PUIProgressWindow got screen class (?P<screen_class>-?\d+)',
        note="ProgressUI's window in backboardd, once at boot: -1 before",
    ),
    line(
        'boot',
        'hid-enqueue-dropped',
        'Event enqueue dropped',
        REPORT,
        process='',
        show=('process',),
        note='O12: read beside the cores line (were the cores free?)',
    ),
    line(
        'boot',
        'prefetch-used',
        'SwitchBootPrefetch: ',
        'max(used) > 546',
        regex=(
            r'^SwitchBootPrefetch: (?P<ahead>\d+) MiB paged in ahead \((?P<used>\d+) used, (?P<unused>\d+) '
            r'reclaimed unused, lead (?P<lead>\d+) of bound (?P<bound>\d+)\)'
        ),
        source='kernel/switch/vm/prefetch.c (big/boot d4c468245)',
        note='lines at 13, 23, 33 and 43 s; used/paged in was 546/1,663 MiB at 43 s',
    ),
    Ref('boot.prefetch-lead'),
    line(
        'boot',
        'sd-hi-wait',
        'SwitchSD: ',
        'max(hi_max) < 131',
        regex=r'\| wait ms hi (?P<hi_n>\d+)/(?P<hi_mean>[\d.]+)/(?P<hi_max>\d+) ',
        source=SD,
        note='hi requests per window, count/mean/max ms: 11.5 mean and 131 max in the slot-3 window before, p90 '
        '33-93 ms in the 12-42 s windows',
    ),
    line(
        'boot',
        'sd-passed',
        'SwitchSD: ',
        SUMS,
        regex=r' passed (?P<passed>\d+)/(?P<passed_total>\d+)',
        show=('passed',),
        source=SD,
        note='should rise',
    ),
    line(
        'boot',
        'logd-biowait-stall',
        'SwitchStall: ',
        'count == 0',
        regex=r'^SwitchStall: logd\[\d+\].*buf_biowait',
        source='kernel/switch/diag/stall.c',
        note="no stall chain through logd's buf_biowait (15-21 s before)",
    ),
    line(
        'boot',
        'band-end',
        'SwitchBootBand: ',
        'count == 0',
        regex=r'60 s of uptime',
        source='kernel/xnu-*/osfmk/kern/switch_boot_band.c (big/boot d4c468245)',
        note='the band ends at System Boot Complete, not at 60 s of uptime',
    ),
    Ref('boot.window-server-ready'),
    Ref('boot.display-query-refused'),
    line(
        'boot',
        'kona-exit',
        'KonaSynthesizer',
        'count == 0',
        process='',
        regex=r'\(7, ?3\)',
        note='no exit reason (7, 3) for KonaSynthesizer in the first 3 minutes',
    ),
    line('boot', 'migration-not-again', 'not migrating again', REPORT, process='', show=('process',)),
    line(
        'boot',
        'lsd-rebuild',
        'rebuildReasonFlags',
        'count == 0',
        process='lsd',
        needs='owed.boot.migration-not-again',
        note='after a deploy over a card with an app the migration could not install',
    ),
    Ref('boot.springboard'),
    line(
        'boot',
        'iosched-mounts',
        'SwitchIOSched: ',
        REPORT,
        regex=r'^SwitchIOSched: (?P<mount>\S+) \((?P<fs>[^,]+), bsd unit',
        show=('mount', 'fs'),
        source='kernel/xnu-*/bsd/vfs/vfs_subr.c (big/boot d4c468245)',
        note='one line per mount that took IOSCHED: the Data volume and the cryptex volumes',
    ),
    line(
        'boot',
        'icon-cache-kept',
        'SwitchRawBoot: icon cache for GPU build',
        TIMELINE,
        regex=r'kept for the previous build, (?P<trash>\d+) moved to trash, (?P<restored>\d+)',
        source='kernel/xnu-12377.121.6/bsd/kern/kern_exec.c (big/boot d4c468245)',
        note='after a deploy back to the build staged before: folders kept, 0 moved to trash, restored',
    ),
    line(
        'boot',
        'icon-cache-no-reset',
        'Resetting icon cache',
        'count == 0',
        process='iconservicesagent',
        needs='owed.boot.icon-cache-kept',
    ),
    row(
        'boot',
        'system.icon_cache_reset',
        'restored_this_boot',
        rule='max(restored_this_boot) >= 1',
        needs='owed.boot.icon-cache-kept',
    ),
    line(
        'boot',
        'appregd-watermark',
        'crossed memory high watermark',
        'count == 0',
        regex=r'process switch-appregd\[\d+\] crossed memory high watermark',
        source='kernel/xnu-*/osfmk/kern/task.c',
    ),
]

DMA = [
    line(
        'dma',
        'mc-error',
        'TegraSMMU: MC error',
        'count == 0',
        regex=r'TegraSMMU: MC error (?P<what>.{0,60})',
        show=('what',),
        source='drivers/TegraSMMU/TegraMCErrors.cpp (big/dma 5b7117aa0)',
        note='a panic: read its report in the crash folder too (kind, client, direction, address)',
    ),
    line('dma', 'invalid-smmu-page', 'INVALID_SMMU_PAGE', 'count == 0', process=''),
    row('dma', 'system.mc_fault_interrupt'),
    line(
        'dma',
        'mc-fault-probe',
        'TegraVIC: MC fault probe: ',
        'min(faults) >= 1',
        regex=r'status (?P<status>-?\d+), (?P<faults>\d+) SMMU faults delivered by the MC error interrupt',
        source='drivers/TegraVIC/SwitchVICMcFaultProbe.cpp (big/dma 5b7117aa0)',
    ),
    line(
        'dma',
        'behind-smmu',
        'DMA through the SMMU',
        REPORT,
        regex=r'^(?P<who>\S+?):? (?:(?P<engine>SE\d|GPU|APE|command) )?DMA through the SMMU',
        show=('who', 'engine'),
        note='boot 1 (tip a): tegra-gm20b GPU, TegraHost1x, SwitchAudio APE, TegraSE SE1 and SE2; boot 2 (tip b) adds '
        'SwitchSD',
    ),
    line(
        'dma',
        'not-behind-smmu',
        'DMA not behind the SMMU',
        'count == 0',
        regex=r'^(?P<who>\S+?):? .*DMA not behind the SMMU \((?P<why>[^)]*)\)',
        show=('who', 'why'),
    ),
    line(
        'dma',
        'gpu-smmu-counters-on',
        'tegra-gm20b: smmu ',
        TIMELINE,
        regex=r'counters on from here \(TLB_CONFIG (?P<tlb>\S+) PTC_CONFIG (?P<ptc>\S+)\)',
        source='drivers/TegraSMMU/TegraSMMU.cpp (big/dma 5b7117aa0)',
        note='the first window; answers review open question 4',
    ),
    line(
        'dma',
        'gpu-smmu-hits',
        'tegra-gm20b: smmu ',
        SUMS,
        regex=(
            r'tlb (?P<tlb_hits>\d+) hits (?P<tlb_misses>\d+) misses \([\d.]+% hit\), ptc (?P<ptc_hits>\d+) hits '
            r'(?P<ptc_misses>\d+) misses.*?retirement: (?P<objects>\d+) objects, (?P<gpu_flushes>\d+) GPU-ordered '
            r'flushes, (?P<invalidates>\d+) CPU-waited fallbacks'
        ),
        show=('tlb_hits', 'tlb_misses', 'ptc_hits', 'ptc_misses', 'gpu_flushes', 'invalidates'),
        source='drivers/TegraGM20B/gpu_stats_tick.c (big/dma 5b7117aa0)',
    ),
    line(
        'dma',
        'sd-smmu-chains',
        'SwitchSD: ',
        SUMS,
        regex=r' smmu (?P<chains>\d+) chains (?P<ns>\d+)ns each',
        show=('chains',),
        source=SD.replace('big/boot d4c468245', 'big/dma 5b7117aa0'),
        note='boot 2: the cost of the SD view during the boot storm',
    ),
    line(
        'dma',
        'sd-throughput',
        'SwitchSD: ',
        SUMS,
        regex=r'^SwitchSD: (?P<rq>\d+) rq (?P<mib>\d+) MiB .*?\| dma (?P<dma>\d+) bounce (?P<bounce>\d+)',
        show=('rq', 'mib', 'dma', 'bounce'),
        source=SD.replace('big/boot d4c468245', 'big/dma 5b7117aa0'),
        note="against boot-1125-1430: a refused hold lands in bounce's mem count",
    ),
    line('dma', 'wake-smmu', 'SwitchSuspend: wake smmu: ', TIMELINE, note='SC7: after a sleep and wake'),
    line('dma', 'wake-smmu-lost', 'a swgroup was not restored', 'count == 0', needs='owed.dma.wake-smmu'),
    line('dma', 'sd-adma-off-after-sleep', 'SwitchSD: ADMA2 off after sleep', 'count == 0'),
    line('dma', 'se2-swgroups', "TegraSE: SE2's swgroups", TIMELINE, note='once the platform lane takes the patch'),
]

DISPLAY_SRC = 'drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp (big/display 56c4c8538)'
DISPLAY = [
    line(
        'display',
        'requested',
        'SwitchDisplay requested: ',
        SUMS,
        regex=(
            r'periods <=(?P<minus>-?\d+):(?P<early_bin>\d+) .*?, unknown (?P<unknown>\d+); timed gate held early '
            r'(?P<held_early>\d+) phase (?P<held_phase>\d+), shown (?P<shown>\d+), replaced while held '
            r'(?P<replaced>\d+), unphased (?P<unphased>\d+), shown early (?P<shown_early>\d+)'
        ),
        show=('early_bin', 'shown_early', 'held_early', 'replaced'),
        source=DISPLAY_SRC,
        note='the -1 bin and shown early at 0 in phased windows, held early up, replaced while held down (32 of 63 at '
        '19:18:47); watch the +1 bin during video playback',
    ),
    line('display', 'locks', 'SwitchDisplay locks: ', TIMELINE, source=DISPLAY_SRC, note='who keeps FRAME_END on'),
    Ref('display.frame-end-idle', 'big/display'),
    row('display', 'display.present_at_time', note='still expected FAIL: timed 0 (D4)'),
    row('display', 'display.media_completion_flip_gated', note='expected FAIL (D5): read b-only in its window'),
    row('display', 'display.x1.landscape_video', note='read the sample lines against the landscape on/off lines'),
]

POWER = [
    line(
        'power',
        'system-time',
        'SwitchSched system (',
        SUMS,
        regex=r'outside a trap (?P<outside_ms>\d+) ms',
        show=('outside_ms',),
        source='kernel/switch/sched/systime.c (big/power bfdeb2e56)',
        note="in every window; buckets plus outside a trap near the tasks line's system sum",
    ),
    line('power', 'pmu', 'SwitchSched pmu (', TIMELINE, note='a busy core with no C7 entries shows PMIs'),
    line('power', 'callouts', 'SwitchSched callouts (', TIMELINE, note="kernel-high's busiest: atos -o kernel.macho"),
    line('power', 'totals', 'SwitchSched totals ', TIMELINE, note='once'),
    line('power', 'build', 'SwitchBuild: ', TIMELINE, source='kernel/switch/diag/deployed.c (big/power bfdeb2e56)'),
    line('power', 'a64x', 'SwitchSched a64x (', TIMELINE, note='names the JIT processes and classes'),
    line('power', 'governor', 'busy by CPU: ', TIMELINE, note='the top-point windows from utility work alone'),
    line('power', 'limits-lifted', 'SwitchSched CPU limits lifted (', TIMELINE),
    line(
        'power',
        'spi4-failed',
        'TegraSPI stats: SPI4 ',
        'max(failed) == 0',
        regex=(
            r'SPI4 (?P<ms>\d+) ms: (?P<blocks>\d+) blocks \(\d+/s\), (?P<packed>\d+) packed, (?P<bytes>\d+) bytes in '
            r'(?P<words>\d+) FIFO words each way, (?P<failed>\d+) failed'
        ),
        source='drivers/TegraSPI/TegraSPI.cpp (big/power bfdeb2e56)',
        note='FIFO words about a quarter of the bytes; blocks per drain 2 at idle',
    ),
    Ref('hwtest.sensor.device_motion'),
    row('power', 'cpu.dfll_landing', 'under_pct_1581', note='under_pct_1581 smaller'),
    line('power', 'dfll-lut', 'TegraDFLL: LUT', TIMELINE),
    line('power', 'battery-from-nvram', "powerd's battery data from NVRAM", TIMELINE, note='the second boot'),
    line('power', 'first-use-fails', 'DateOfFirstUse', 'count == 0', process='powerd', regex=r'(?i)fail'),
    line(
        'power',
        'vic-clock',
        'TegraVIC: V1 ok ',
        'min(khz) >= 422400',
        regex=r', (?:(?P<khz>\d+) kHz|(?P<mhz>\d+) MHz), falcon started',
        source='drivers/TegraVIC/TegraVIC.cpp (big/power bfdeb2e56)',
        note="PLLC2 for NVJPG and VIC at 422400 kHz, L4T's 637 mV point",
    ),
    line('power', 'pllc2', 'PLLC2 for NVJPG and VIC', TIMELINE),
    Ref('hwtest.gpu.x1.gpu_vic_sync'),
]

HW2 = 'apps/hwtest (big/hwtest2 ba861185b)'
HWTEST2 = [
    row('hwtest2', 'suite.harness', 'boundary_ms_p50', rule='max(boundary_ms_p50) < 100', source=HW2),
    row('hwtest2', 'suite.harness', 'main_wait_ms_p50', rule='max(main_wait_ms_p50) < 16.7', source=HW2),
    row('hwtest2', 'suite.harness', 'main_late', rule='max(main_late) < 662', source=HW2),
    row('hwtest2', 'suite.harness', 'main_cpu_ms_p50', rule='max(main_cpu_ms_p50) < 10', source=HW2),
    row(
        'hwtest2',
        'suite.harness',
        words=('over_budget_ids',),
        source=HW2,
        note='only glass_sidebar, glass_live and the fixed scenarios',
    ),
    row('hwtest2', 'gpu.gm20b.compressed_targets', words=('zbc_free',), source=HW2),
    none_of('hwtest2', 'gpu.gm20b.compressed_targets', r'zbc_\w*_table_full', name='table-full', source=HW2),
    row('hwtest2', 'display.media_completion_flip', source=HW2, note='FAIL "every counter stayed 0" if S22 holds'),
    row('hwtest2', 'system.overlay_apps', source=HW2, note='judged, no longer SKIP'),
    row('hwtest2', 'system.overlay_app_records', source=HW2, note='judged, no longer SKIP'),
    row('hwtest2', 'ui.icon_render', source=HW2, note='inactive_limit_mb, settled_excess_mb'),
    none_of(
        'hwtest2', 'ui.icon_render', r'memorystatus_errno', name='sandbox', source=HW2, note='the sandbox refuses 13'
    ),
    row('hwtest2', 'gpu.present.copy_reset', 'copy_held', rule='min(copy_held) >= 1', source=HW2),
    row('hwtest2', 'gpu.recover.kernel_second_timeout', source=HW2, note='copy_held=1 and judged'),
    row('hwtest2', 'gpu.poster_targets', source=HW2, note='owners, peaks; SKIP until the scan sees the poster'),
    row('hwtest2', 'gpu.path.cpu_locks_side_by_side', source=HW2, note='judged against a_gpu_ms'),
    row('hwtest2', 'gpu.iosurface.cross_process', source=HW2, note='*_explained for 256 and 260'),
    row('hwtest2', 'system.snapshot_purgeable', source=HW2, note='marks_*; an old card fails marks_unsupported'),
    line(
        'hwtest2',
        'long-tests',
        'HWTEST ',
        'count == 0',
        process='',
        subsystem='com.brunostjohn.switch.hwtest',
        regex=(
            r'^HWTEST (?P<id>(?!ui\.glass_sidebar |ui\.glass_live |suite )\S+) (?:PASS|FAIL|SKIP)\b.* '
            r'ms=(?P<ms>(?:1[5-9]|[2-9]\d|\d{3,})\d{3}(?:\.\d)?)(?: why=|$)'
        ),
        show=('id',),
        source=HW2,
        note='every test under 15 s but ui.glass_sidebar (about 38.6 s) and ui.glass_live (about 29.5 s)',
    ),
    row('hwtest2', 'gpu.gm20b.standin_ms_resolve', source=HW2),
    row('hwtest2', 'gpu.gm20b.resolve_twod', source=HW2),
]

MEASURE = [
    line('measure', 'store', 'SwitchRecorder: store of ', TIMELINE, source='kernel/switch/recorder/store.c'),
    line(
        'measure',
        'presses',
        'Snapshot button pressed down',
        REPORT,
        process='SpringBoard',
        subsystem='com.apple.SpringBoard',
        note="SpringBoard's own line for a Capture press",
    ),
    line(
        'measure',
        'press-captures',
        'SwitchRecorder: capture ',
        'count >= 1',
        needs='owed.measure.presses',
        regex=r'^SwitchRecorder: capture (?P<seq>\d+) \(mark ask .*? in memory \+(?P<memory_us>\d+) us',
        source='kernel/switch/recorder/capture.c',
        note='one per Capture press (runbook step 3)',
    ),
    line(
        'measure',
        'capture-latency',
        'SwitchRecorder: capture ',
        'p50(memory_us) < 100000',
        regex=r' in memory \+(?P<memory_us>\d+) us',
        source='kernel/switch/recorder/capture.c',
        note='a capture in memory within 100 ms of its trigger once the boot settles',
    ),
    line(
        'measure',
        'no-room',
        'dropped: the captures waiting for the store fill its memory',
        REPORT,
        source='kernel/switch/recorder/capture.c',
        note='automatic captures dropped at boot, before their stackshot',
    ),
    Ref('hwtest.recorder.hang_dump'),
    line(
        'measure',
        'labels',
        'SwitchLabels: ',
        TIMELINE,
        process='SpringBoard',
        subsystem=METAL,
        source='drivers/TegraGM20BMetal/TegraGM20BMetalPlugin.mm',
        note='"icon label image N set at T", or the one line naming a missing selector',
    ),
    line('measure', 'kprof-boot', 'SwitchKprof: boot window over', TIMELINE, note='240 s into a recorder boot'),
    line(
        'measure',
        'longest-began',
        'SwitchSched 1/2',
        REPORT,
        regex=r'(?P<thread>[\w.\-]+)\[\d+\] [^;]*?, longest began (?P<before>\d+) ms before the window',
        show=('thread',),
        source='kernel/switch/sched/stats.h',
        note='on entries whose wait spans windows, never on every entry',
    ),
    Ref('sched.backboardd-main-longest'),
    Ref('media.host1x-channel-counters'),
    line(
        'measure',
        'host1x-stalled',
        'TegraHost1x: ',
        'max(vic_stalled) > 0',
        regex=(
            r' clocked=\d+/(?P<vic>\d+)/\d+/\d+/(?P<dc>\d+)us '
            r'stalled=\d+/(?P<vic_stalled>\d+)/\d+/\d+/(?P<dc_s>\d+)us'
        ),
        source='drivers/TegraHost1x/SwitchHost1xGatherStats.h',
        note='VIC waits on its producer during gpu.x1.gpu_vic_sync; DC during display.x1.host1x_flip',
    ),
]

LANES = {
    'frame': FRAME,
    'boot': BOOT,
    'dma': DMA,
    'display': DISPLAY,
    'power': POWER,
    'hwtest2': HWTEST2,
    'measure': MEASURE,
}
