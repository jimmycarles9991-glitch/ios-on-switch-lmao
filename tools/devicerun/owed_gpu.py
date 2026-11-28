"""The device checks owed by the GPU lanes (owed.py).

The lanes are state, memory, gputhroughput, gpuformats, gpurender, fetchmsaa, shadergen, devicegates, nohash and
nvcompiler. `source` names the branch tip a format was read at (25-26 Nov).
"""

from tools.devicerun.owed import METAL, Ref, line, none_of, row
from tools.devicerun.rules import REPORT, SUMS, TIMELINE

CREATION = (
    r'^SwitchMetal: \S+: render state [0-9a-f]{16} from the pack in (?P<wall_us>\d+) us: key \d+ us, place '
    r'(?P<place_us>\d+) us \(.*?(?P<builds>\d+) builds (?P<build_us>\d+) us.*?pageins (?P<pageins>\d+)'
)
STATE_SRC = 'drivers/TegraGM20BMetal/TegraGM20BMetalStatesCost.mm (big/state 20d167f5b)'
STATE = [
    line(
        'state',
        'backboardd-place',
        ': render state ',
        'p50(place_us) <= 300',
        process='backboardd',
        subsystem=METAL,
        regex=CREATION,
        source=STATE_SRC,
        note='a pack hit places in about its builds: p50 2.9-5.5 ms before',
    ),
    line(
        'state',
        'backboardd-pageins',
        ': render state ',
        'max(pageins) == 0',
        process='backboardd',
        subsystem=METAL,
        regex=CREATION,
        source=STATE_SRC,
        note='no page-in during a tier-0 hit',
    ),
    row('state', 'gpu.pack.hit_cost', note='first p99 near its p50 (76 ms before); repeat unchanged'),
    line(
        'state',
        'rtsync',
        ' rtsync=',
        SUMS,
        process='backboardd',
        subsystem=METAL,
        regex=r'\brtsync=(?P<rtsync>\d+)/(?P<rtsync_us>\d+)us',
        show=('rtsync', 'rtsync_us'),
        source='drivers/TegraGM20BMetal/TegraGM20BMetalRates.mm (big/state 20d167f5b)',
        note='render-thread sync creations: the count unchanged, their time down by the place time',
    ),
    line('state', 'no-pack-warm', 'tegra-gm20b: pack warm: ', 'count == 0', note='the tier-1 warm is gone'),
    line(
        'state',
        'pack-pageins',
        'SwitchSched page-ins by file',
        REPORT,
        regex=r'\bpack\.bin\b',
        note='about 0 pack.bin reads after the tier-0 image',
    ),
    line(
        'state',
        'pack-lacks',
        'state the pack lacks',
        REPORT,
        process='',
        subsystem=METAL,
        regex=r'^SwitchMetal: (?P<proc>\S+): a render (?:variant )?state the pack lacks (?P<key>[0-9a-f]{16})',
        show=('proc',),
        note='one line per key: feed logharvest',
    ),
    Ref('boot.ca-no-catch-all', 'big/state'),
    Ref('boot.ca-pack-covers-all', 'big/state'),
]

SPARES = 'drivers/TegraGM20B/TegraGM20BUserClientSpares.cpp (big/memory d622fcd9e)'
MISSES = r'pressure (?P<pressure>\d+), .*?refill refused (?P<refused>\d+), not in 128 KiB units (?P<units>\d+)'
MEMORY = [
    line(
        'memory',
        'refill-refused',
        'tegra-gm20b: mem3 ',
        'max(refused) == 0',
        regex=MISSES,
        where='pressure == 0',
        source=SPARES,
        note='0 outside pressure',
    ),
    line(
        'memory',
        'spares',
        'tegra-gm20b: mem ',
        'p50(kib) >= 92160',
        regex=r'; spares (?P<kib>\d+) KiB in (?P<n>\d+),',
        source='drivers/TegraGM20B/gpu_stats.c (big/memory d622fcd9e)',
        note='about 96 MiB of spares from the first window (90 MiB is the floor here)',
    ),
    line(
        'memory',
        'backboardd-made',
        'tegra-gm20b: mem3 ',
        SUMS,
        regex=r'\bbackboardd (?P<n>\d+) \((?P<kib>\d+) KiB, \d+/\d+ us, made (?P<made>\d+)\)',
        show=('n', 'made'),
        source=SPARES,
        note="backboardd's fresh BoAllocs and those made on its thread: made drops in display tests",
    ),
    line(
        'memory',
        'reserved',
        'tegra-gm20b: mem3 ',
        SUMS,
        regex=r'ineligible \d+, reserved (?P<reserved>\d+);',
        show=('reserved',),
        source=SPARES,
        note='misses the 24 MiB reserve counts',
    ),
    line(
        'memory',
        'refill-step',
        'the spare refill could not make a',
        'count == 0',
        regex=r'could not make a (?P<kib>\d+) KiB spare: (?P<step>[^;]+)',
        show=('step',),
        source=SPARES,
        note='its first line names the step',
    ),
    row('memory', 'gpu.path.launch_target', 'slowest_us', rule='max(slowest_us) < 1500'),
    row('memory', 'gpu.path.idle_redraw', words=('slowest',), note='heap_restore back near 479 us (slowest_us)'),
    Ref('hwtest.gpu.path.job_visibility'),
    row('memory', 'gpu.path.parked_bo'),
    line(
        'memory',
        'chunk-unpark',
        'SwitchMetal: chunk unpark ',
        TIMELINE,
        process='',
        subsystem=METAL,
        source='drivers/TegraGM20BMetal/parked_chunks.cpp (big/memory d622fcd9e)',
    ),
    row('memory', 'gpu.path.fresh_spare', note='stays red: system spares are not built'),
    line(
        'memory',
        'not-in-units',
        'tegra-gm20b: mem3 ',
        'max(units) == 0',
        regex=MISSES,
        source=SPARES,
        note='a refused prefaulted mapping would show here',
    ),
    line('memory', 'zero-fill-failed', "the copy engine's zero-fill of a", 'count == 0', source=SPARES),
    line('memory', 'cached-zero-panic', 'switch_gpu_cached_zero', 'count == 0', note='and no panic report naming it'),
]

GPUT = 'apps/hwtest (big/gputhroughput 255734fc5)'
GPUTHROUGHPUT = [
    row(
        'gputhroughput',
        'metal.compute',
        'gpu_ms',
        rule='max(gpu_ms) < 50',
        source=GPUT,
        note='437-449 ms before, about 1.6 ms expected',
    ),
    row(
        'gputhroughput',
        'gpu.gm20b.zcull',
        'ms',
        rule='max(ms) < 20000',
        source=GPUT,
        note='53,774 ms before, about 6,300 expected',
    ),
    row(
        'gputhroughput',
        'gpu.ca.hot_states',
        'gm20b_us_pbgraxm_timgbvcma2xhfn_isrc',
        rule='max(gm20b_us_pbgraxm_timgbvcma2xhfn_isrc) < 763',
        source=GPUT,
        note='toward 451 us',
    ),
    row(
        'gputhroughput',
        'gpu.perf.rates',
        source=GPUT,
        note='cached against volatile for atomics, fetches, tables and jobs; lane_mhz near 921.6 (768 handheld)',
    ),
    Ref('hwtest.gpu.path.job_visibility'),
    line(
        'gputhroughput',
        'maps-jobs',
        'tegra-gm20b: jobs Maps',
        TIMELINE,
        regex=r'^tegra-gm20b: jobs Maps: (?P<jobs>\d+) jobs, .*?GPU mean (?P<mean_us>\d+) max (?P<max_us>\d+) us',
        note='42-48 ms a job while panning before',
    ),
    line(
        'gputhroughput',
        'useadd',
        ' useadd=',
        SUMS,
        process='',
        subsystem=METAL,
        regex=r'\buseadd=(?P<useadd>\d+)',
        show=('useadd',),
        note='up for apps: chunks with a Shared buffer leave the resource set',
    ),
    line(
        'gputhroughput',
        'zcull-passes',
        ' zc=',
        REPORT,
        process='',
        subsystem=METAL,
        regex=r'\bzc=(?P<zc>\d+)',
        show=('process', 'zc'),
        note='Maps and Weather pass with a region',
    ),
]

FORMATS = 'apps/hwtest (big/gpuformats 5e941be45)'
GPUFORMATS = [
    row('gpuformats', 'gpu.gm20b.planes_blits', 'skipped_calls', rule='max(skipped_calls) == 96', source=FORMATS),
    none_of(
        'gpuformats',
        'gpu.gm20b.planes_blits',
        r'c_504_exact_rgba(?:16|32)u_changed|cases_off',
        needs='owed.gpuformats.gpu.gm20b.planes_blits.skipped_calls',
        source=FORMATS,
    ),
    row('gpuformats', 'gpu.iosurface.cross_process', source=FORMATS),
    none_of('gpuformats', 'gpu.iosurface.cross_process', r'nvjpg2(?:56|60)_\w*_wrong', source=FORMATS),
    line(
        'gpuformats',
        'nvjpg-route',
        'decode by copy engine into blocks',
        'count >= 1',
        process='',
        needs='owed.gpuformats.gpu.iosurface.cross_process',
        note='the device has never run COPY_BLOCKS',
    ),
    row('gpuformats', 'gpu.packed_plane_target', source=FORMATS),
    none_of('gpuformats', 'gpu.packed_plane_target', r'wrong_(?:words|reads)_588\w*', source=FORMATS),
    row('gpuformats', 'gpu.gm20b.ycbcr_target_shapes', source=FORMATS),
    none_of('gpuformats', 'gpu.gm20b.ycbcr_target_shapes', r'plane_one_bytes_off', source=FORMATS),
    row(
        'gpuformats',
        'gpu.gm20b.resolve_twod',
        'bgra8srgb_4x_ms',
        rule='max(bgra8srgb_4x_ms) < 38.5',
        source=FORMATS,
        note='well under 38.5 ms, texel judgements unchanged',
    ),
    row('gpuformats', 'gpu.gm20b.resolve_twod', 'rgba8srgb_4x_ms', rule='max(rgba8srgb_4x_ms) < 38.5', source=FORMATS),
    row('gpuformats', 'gpu.gm20b.xr_beside_native', source=FORMATS),
    none_of('gpuformats', 'gpu.gm20b.xr_beside_native', r'words_off|sampled_off', source=FORMATS),
    line(
        'gpuformats',
        'jpeg-routes',
        'HWTEST image.jpeg_',
        REPORT,
        process='',
        subsystem='com.brunostjohn.switch.hwtest',
        regex=r'^HWTEST (?P<id>image\.jpeg_\S+) (?P<verdict>PASS|FAIL|SKIP)',
        show=('id', 'verdict'),
        note='routes unchanged for pitch destinations (decode by rows)',
    ),
]

GPURENDER = [
    row('gpurender', 'gpu.gm20b.zcull', 'loaded_passes', rule='min(loaded_passes) == 2'),
    row('gpurender', 'gpu.gm20b.zcull', 'loaded_loads', rule='min(loaded_loads) == 1'),
    row('gpurender', 'gpu.gm20b.blend_kill', 'kill_states', rule='min(kill_states) > 0'),
    row('gpurender', 'gpu.gm20b.blend_kill', 'kill_binds', rule='min(kill_binds) > 0'),
    row('gpurender', 'gpu.gm20b.aegir_moon', note="which arm is red decides the moon's side"),
    Ref('hwtest.gpu.x1.blend_kills'),
]

FETCHMSAA = [
    row(
        'fetchmsaa',
        'gpu.gm20b.maps_fetch',
        note='us_hillshade_fetch against _rop, us_glow_*, the 4x twins, resolve_us (about 12,000 before), plain_us',
    ),
    row('fetchmsaa', 'gpu.msaa.resolve_cost', note='unchanged: unorm formats only'),
    line(
        'fetchmsaa',
        'fetch-route',
        'SwitchMetal: fetch route k',
        TIMELINE,
        process='',
        subsystem=METAL,
        debug=True,
        note="once per state: Hillshade reads 'no fetch-free form ... not a shape measured cheaper'",
    ),
]

SHADERGEN = [
    row(
        'shadergen',
        'gpu.gm20b.half_min_max',
        'wrong',
        rule='max(wrong) == 0',
        note="of 400; a failure: revert 29133da8c (E's nak_nir.c and rules) first, then 84893121c",
    ),
    row(
        'shadergen',
        'gpu.gm20b.powr_specials',
        'wrong_special',
        rule='max(wrong_special) == 0',
        note='a failure: revert edb7c9918',
    ),
    row(
        'shadergen',
        'gpu.ca.sweep',
        note="the 85 states against the Mac's goldens: 63/16/6 exact/close/coarse or better",
    ),
    row('shadergen', 'gpu.ca.hot_states', note='gm20b_us_* for Tn15/Tn27 lower'),
    line(
        'shadergen',
        'compositor-span',
        'long span ',
        'p50(us) <= 3400',
        process='backboardd',
        subsystem=METAL,
        regex=r'long span (?P<us>\d+) us k3ef3a99e4f382ce1 ',
        note='6.04-6.27 ms before for x3 921 kpx; about 2.9-3.4 ms expected',
    ),
]

COMPILER = 'drivers/TegraGM20BMetal/compiler/compiler_service.mm'
DEVICEGATES = [
    line(
        'devicegates',
        'request-not-taken',
        "'s request not taken: ",
        'count == 0',
        process='',
        subsystem=METAL,
        regex=r"SwitchShaderCompiler: pid (?P<pid>\d+)'s request not taken: (?P<why>.{0,60})",
        show=('why',),
        source=COMPILER + ' (big/devicegates 96c7834d2)',
        note='never for app libraries',
    ),
    line(
        'devicegates',
        'store-registered',
        'SwitchShaderCompiler: store registered at ',
        TIMELINE,
        process='',
        subsystem=METAL,
        source=COMPILER + ' (big/devicegates 96c7834d2)',
        note='0 entries on the first boot after this build, then rising',
    ),
    line(
        'devicegates',
        'made-again',
        'states an earlier build made are made again in the background',
        TIMELINE,
        process='',
        subsystem=METAL,
        source=COMPILER + ' (big/devicegates 96c7834d2)',
    ),
]

NOHASH = [
    line(
        'nohash',
        'compiler-start',
        ('SwitchShaderCompiler: store registered at ', 'SwitchShaderCompiler: serving at '),
        TIMELINE,
        process='',
        subsystem=METAL,
        source=COMPILER + ' (big/nohash e3b138a49)',
        note='registered to serving: 20,973 to 21,068 ms on 25 Nov 19:16',
    ),
    line(
        'nohash',
        'jobs',
        'SwitchShaderCompiler: ',
        SUMS,
        process='',
        subsystem=METAL,
        regex=r' queued \d+ ms \(prepare (?P<prepare_us>\d+) us, pool \d+ ms;.*? store (?P<store_ms>\d+) ms: ',
        show=('prepare_us', 'store_ms'),
        source='drivers/TegraGM20BMetal/compiler/compiler_debug.mm (big/nohash e3b138a49)',
        note='prepare down by the client-library re-hash; store 36 ms over 929 jobs on 25 Nov',
    ),
    line(
        'nohash',
        'store-not-synced',
        'SwitchShaderCompiler: the store not synced',
        'count == 0',
        process='',
        subsystem=METAL,
        source=COMPILER + ' (big/nohash e3b138a49)',
        note='after the hard-reset check (runbook): no unsynced store, no GPU fault',
    ),
]

NVCOMPILER = [
    row('nvcompiler', f'gpu.compiler_ab.{key}', note=f'{name}: NAK unsplit against NVIDIA R32.7.6, four seeds')
    for key, name in (
        ('3ef3a99e4f382ce1', 'Compositor XR'),
        ('ae6d01e7cac0d964', 'Hillshade'),
        ('17d6ade58fef2152', 'RenderBox'),
        ('357f8355b68acebd', 'GlowAlpha'),
        ('3f9f7f04b2b91507', 'PatternedRibbon'),
    )
]

LANES = {
    'state': STATE,
    'memory': MEMORY,
    'gputhroughput': GPUTHROUGHPUT,
    'gpuformats': GPUFORMATS,
    'gpurender': GPURENDER,
    'fetchmsaa': FETCHMSAA,
    'shadergen': SHADERGEN,
    'devicegates': DEVICEGATES,
    'nohash': NOHASH,
    'nvcompiler': NVCOMPILER,
}
