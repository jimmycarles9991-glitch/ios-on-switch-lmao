"""Every owed check's regex against a line made from its format string, and the per-lane rows."""

import re
import unittest

from tools.devicerun import checks, owed, owed_all
from tools.devicerun.rules import FAIL, INFO, NA, NO_DATA, PASS, REPORT, Check, Result

SD = (
    'SwitchSD: 12 rq 3 MiB avg 256K | busy 0.1s copy 1ms sync 0ms 30.1 MB/s (rd 30.1 wr 0.0) 5% of 10s | dma 12 '
    'bounce 2 (mem 2) seg 1.0 part 0/0K wmax 512K | <=4K 10% seq 50% merged 1 gap 0/0K q4 | cmds 12 c23 0 irq 12 ovl '
    '0/0 smmu 4 chains 900ns each | err 0 adma 0 retune 0 | SDR104 200.00 MHz | wait ms hi 3/1.5/4 def 2/0.5/1 lo '
    '0/0.0/0 bg 0/0.0/0 passed 1/5 reprio xnu 36/2 cryptex 4/1'
)
MEM3 = (
    'tegra-gm20b: mem3 BoAllocs by process: backboardd 10 (640 KiB, 30/90 us, made 2); spare misses: first 0, '
    'zeroing 1, drained 0, pressure 0, ineligible 0, reserved 3; copy-engine zero busy 0, failed 0; refill refused 0, '
    'not in 128 KiB units 0'
)
CREATION = (
    'SwitchMetal: backboardd: render state 3ef3a99e4f382ce1 from the pack in 400 us: key 10 us, place 250 us (3 '
    'lookups 5 us, 2 pieces 6 us, 1 copies 64 bytes, 2 builds 200 us, 1 variants), packed variants 10 us, record 20 '
    'us, other 5 us; 0 text keys; 1 locks, 0 waited 0 us; 2 kernel calls 30 us; task faults 0, pageins 0: label'
)
HOST1X = (
    'TegraHost1x: 5s NVDEC/VIC/NVENC/NVJPG/DC gathered=0/40/0/0/60 copied=0/0/0/0/0 arena_high=0/3/0/0/1KB '
    'room_waits=0/0/0/0/0 clocked=0/5000000/0/0/5000000us stalled=0/1000000/0/0/4500000us'
)
# One line per check, made from the format string its `source` names, as the device writes it.
SAMPLES = {
    'owed.frame.early-schedule': (
        'TegraGM20BMetal: backboardd frame pass=1 hit=2 kept=3 unkept=0 unkeyed=0 counters=1 owned=2 split=0 '
        'kick=0/0us ahead=0/10 enc=0/0 why=- cbs=1/1/1/1 encoders=1/1/1/1 park=- ws=4/120us(queued 1 parked 0 '
        'encoding 0 handlers 0 other 0) wsmax=50us(1 2 3 4 5) early=5/1/0/2/0/0/1 enq=1/2/3'
    ),
    'owed.frame.early-promise-refused': (
        "SwitchMetal: the kernel refused the render server's early writer promise for 2 surfaces (0xe00002c2): its "
        'frames wait for their encode'
    ),
    'owed.boot.sd-reprio': SD,
    'owed.boot.sd-hi-wait': SD,
    'owed.boot.sd-passed': SD,
    'owed.dma.sd-smmu-chains': SD,
    'owed.dma.sd-throughput': SD,
    'owed.boot.cryptex-set-priority': (
        'SwitchCryptex: 10000 ms: 5 completions, client 3 us (max 9 us); copied 64 KiB in 30 us, 0 KiB of it waited '
        'for; direct 0 KiB; card into windows 0 KiB; raised to high 0 KiB; raises since boot: joined 2, doSetPriority '
        '0, late 1'
    ),
    'owed.boot.backboardd-startup': 'startup complete. 20.5702s elapsed',
    'owed.boot.progressui-screen-class': 'PUIProgressWindow got screen class -1',
    'owed.boot.prefetch-used': (
        'SwitchBootPrefetch: 1663 MiB paged in ahead (546 used, 0 reclaimed unused, lead 15 of bound 60), 12 MiB of '
        'demand reads by volume (1/2/9), card ms 100/200, requests > 64 KiB replay 1/2, demand 3/4'
    ),
    'owed.boot.logd-biowait-stall': 'SwitchStall: logd[45] main thread 0x1234 blocked 2000 ms in buf_biowait',
    'owed.boot.band-end': 'SwitchBootBand: off at 60 s of uptime',
    'owed.boot.kona-exit': 'KonaSynthesizer[300] exited: exit reason (7, 3)',
    'owed.boot.iosched-mounts': (
        'SwitchIOSched: disk3s1 (apfs, bsd unit 3) schedules I/O by tier, software throttle kept'
    ),
    'owed.boot.icon-cache-kept': (
        'SwitchRawBoot: icon cache for GPU build abc123: 2 folder(s) kept for the previous build, 0 moved to trash, 2 '
        'restored'
    ),
    'owed.boot.appregd-watermark': 'process switch-appregd[88] crossed memory high watermark (6 MB); EXC_RESOURCE',
    'owed.dma.mc-error': (
        'panic: TegraSMMU: MC error interrupt (INTID 109) masked: INTMASK reads 0x0 after 0x33d40 was written'
    ),
    'owed.dma.mc-fault-probe': (
        'TegraVIC: MC fault probe: copy from 0xdead0000 status -536870911, 1 SMMU faults delivered by the MC error '
        'interrupt (attached 1), the first INTSTATUS 0x10000 ERR_STATUS 0x1 ERR_ADR 0xdead0000'
    ),
    'owed.dma.behind-smmu': 'TegraSE: SE1 DMA through the SMMU (MC 0xabc and 0xac8): it reaches its buffers',
    'owed.dma.not-behind-smmu': 'SwitchSD: DMA not behind the SMMU (no view): the card can reach all of RAM',
    'owed.dma.gpu-smmu-counters-on': (
        'tegra-gm20b: smmu counters on from here (TLB_CONFIG 0x30000020 PTC_CONFIG 0x2800003f); retirement L2 '
        'invalidates 0 for 0 objects, 0 us CPU'
    ),
    'owed.dma.gpu-smmu-hits': (
        'tegra-gm20b: smmu tlb 100 hits 5 misses (95.2% hit), ptc 200 hits 10 misses (95.2% hit); '
        'retirement: 4 objects, 7 GPU-ordered flushes, 3 CPU-waited fallbacks (12 us)'
    ),
    'owed.display.requested': (
        'SwitchDisplay requested: 5000 ms: shown minus requested presentation, periods <=-1:0 0:290 1:3 >=2:1, '
        'unknown 0; timed gate held early 4 phase 2, shown 5, replaced while held 1, unphased 0, shown early 0'
    ),
    'owed.power.system-time': (
        "SwitchSched system (10000 ms; user threads' kernel time by what entered the kernel): unix 3200 ms (412000), "
        'mach_msg 2100 ms (95000), outside a trap 1800 ms'
    ),
    'owed.power.spi4-failed': (
        'TegraSPI stats: SPI4 5000 ms: 100 blocks (20/s), 50 packed, 4000 bytes in 1000 FIFO words each way, 0 failed'
    ),
    'owed.power.first-use-fails': 'powerd: writing DateOfFirstUse failed (5)',
    'owed.power.vic-clock': (
        'TegraVIC: V1 ok ASID 11 (directory 0x80000000), partition 2 on, 422400 kHz, falcon started (OS code 0x0+0x100'
    ),
    'owed.hwtest2.long-tests': 'HWTEST gpu.gm20b.planes_blits PASS cases=4 ms=16000.0',
    'owed.measure.press-captures': (
        'SwitchRecorder: capture 3 (mark ask 5, thread 0, value 0) at 123: stackshot 900 KiB in 2900 us (+40 us), '
        'ring 16000 KiB in 50000 us (+3000 us), in memory +60000 us; its stretch: on a core 0 ms, blocked 0 ms, '
        'waiting for a core 0 ms'
    ),
    'owed.measure.capture-latency': (
        'SwitchRecorder: capture 4 (hang ask 6, thread 514, value 300) at 900: stackshot 900 KiB in 2900 us (+40 '
        'us), ring 9000 KiB in 40000 us (+2900 us), in memory +45000 us; its stretch: on a core 120 ms'
    ),
    'owed.measure.longest-began': (
        'SwitchSched 1/2 (10000 ms; runnable-to-running us): backboardd.main[69] 53/53 TS IN FG: 1 runs '
        '32556000/32556000/32556000 us, longest began 23000 ms before the window; render-server[69] 55/55 FX - FIX'
    ),
    'owed.measure.host1x-stalled': HOST1X,
    'owed.state.backboardd-place': CREATION,
    'owed.state.backboardd-pageins': CREATION,
    'owed.state.rtsync': 'TegraGM20BMetal: backboardd gm20b 5s c=120(3) cag=0 rtsync=3/150us',
    'owed.state.pack-pageins': 'SwitchSched page-ins by file (10000 ms): pack.bin 3/192/4/0/0',
    'owed.state.pack-lacks': 'SwitchMetal: backboardd: a render state the pack lacks 0123456789abcdef (label)',
    'owed.memory.refill-refused': MEM3,
    'owed.memory.not-in-units': MEM3,
    'owed.memory.backboardd-made': MEM3,
    'owed.memory.reserved': MEM3,
    'owed.memory.spares': (
        'tegra-gm20b: mem BoAlloc spare 3 (192 KiB) mean 10 us max 20 us, cached 0 (0 KiB) mean 0 us max 0 us (-) '
        '[alloc+wire 0, clean 0, pages 0, nvkm 0 us], pool 0 (0 KiB) mean 0 us max 0 us, 0 of <= 64 KiB; spares '
        '98304 KiB in 768, pool 0 KiB zeroing; BoFree 1 mean 3 us max 3 us; 0 prefaulted maps (0 KiB); faults 0/s '
        '(0/s zero-fill)'
    ),
    'owed.memory.refill-step': (
        'tegra-gm20b: the spare refill could not make a 128 KiB spare: the VM gave no pages; further ones not logged'
    ),
    'owed.gputhroughput.maps-jobs': 'tegra-gm20b: jobs Maps: 120 jobs, 5000 us, GPU mean 42000 max 90000 us',
    'owed.gputhroughput.useadd': 'TegraGM20BMetal: Maps encode 5s c=20 useadd=5 enc=300us/c',
    'owed.gputhroughput.zcull-passes': 'TegraGM20BMetal: Maps gm20b 5s c=20(1) zc=3',
    'owed.gpuformats.jpeg-routes': 'HWTEST image.jpeg_after_idle PASS route=rows ms=12.0',
    'owed.shadergen.compositor-span': (
        'SwitchMetal: backboardd: long span 6100 us k3ef3a99e4f382ce1 x3 921 kpx in a 9000 us command buffer'
    ),
    'owed.devicegates.request-not-taken': "SwitchShaderCompiler: pid 412's request not taken: an app library",
    'owed.nohash.jobs': (
        'SwitchShaderCompiler: made in 120 ms, queued 3 ms (prepare 450 us, pool 1 ms; class 0x2) for pid 70: '
        'libraries 10 ms, translation and record 50 ms (0 more translations kept), stage compiles 60 ms (2 of 2 '
        'made), store 1 ms: key'
    ),
}
ZERO_SAMPLES = {
    'owed.gpuformats.gpu.gm20b.planes_blits.zero': (
        'HWTEST gpu.gm20b.planes_blits FAIL c_504_exact_rgba16u_changed=3 ms=1.0'
    ),
    'owed.gpuformats.gpu.iosurface.cross_process.zero': (
        'HWTEST gpu.iosurface.cross_process FAIL nvjpg256_y_wrong=2 ms=1.0'
    ),
    'owed.gpuformats.gpu.packed_plane_target.zero': 'HWTEST gpu.packed_plane_target FAIL wrong_words_588_a=1 ms=1.0',
    'owed.gpuformats.gpu.gm20b.ycbcr_target_shapes.zero': (
        'HWTEST gpu.gm20b.ycbcr_target_shapes FAIL plane_one_bytes_off=8 ms=1.0'
    ),
    'owed.gpuformats.gpu.gm20b.xr_beside_native.zero': 'HWTEST gpu.gm20b.xr_beside_native FAIL sampled_off=4 ms=1.0',
    'owed.hwtest2.gpu.gm20b.compressed_targets.table-full': (
        'HWTEST gpu.gm20b.compressed_targets PASS zbc_free=2 zbc_depth_table_full=1 ms=1.0'
    ),
    'owed.hwtest2.ui.icon_render.sandbox': 'HWTEST ui.icon_render FAIL memorystatus_errno=1 ms=1.0',
}


def owed_checks() -> list[Check]:
    return [c for lane in owed_all.LANES.values() for c in owed.checks(lane)]


class OwedTest(unittest.TestCase):
    def test_every_line_check_matches_a_line_made_from_its_format(self):
        lines = [c for c in owed_checks() if c.regex and not c.literals()[0].startswith('HWTEST ')]
        self.assertEqual(sorted(c.name for c in lines if c.name not in SAMPLES), [])
        for c in lines:
            sample = SAMPLES[c.name]
            self.assertTrue(any(literal in sample for literal in c.literals()), c.name)
            m = re.search(c.regex, sample)
            self.assertIsNotNone(m, c.name)
            for field in c.show:
                if field != 'process':
                    self.assertIn(field, m.groupdict(), c.name)

    def test_every_hwtest_row_matches_its_line_with_its_fields(self):
        for c in owed_checks():
            if not c.literals()[0].startswith('HWTEST ') or '(?P<field>' in c.regex or c.name in SAMPLES:
                continue
            test = c.literals()[0].split()[1]
            fields = re.findall(r'\(\?=\.\*\\b(\w+)=\(\?P<\1>', c.regex)
            sample = f'HWTEST {test} PASS ' + ' '.join(f'{f}=1' for f in fields) + ' ms=2.0'
            self.assertIsNotNone(re.search(c.regex, sample), c.name)
            self.assertIsNone(re.search(c.regex, f'HWTEST {test}x PASS ms=2.0'), c.name)

    def test_a_none_of_check_finds_a_field_above_zero_and_not_one_at_zero(self):
        by_name = {c.name: c for c in owed_checks()}
        self.assertEqual(sorted(n for n, c in by_name.items() if '(?P<field>' in c.regex), sorted(ZERO_SAMPLES))
        for name, sample in ZERO_SAMPLES.items():
            c = by_name[name]
            self.assertIsNotNone(re.search(c.regex, sample), name)
            self.assertIsNone(
                re.search(c.regex, re.sub(r'=\d+ ms=', '=0 ms=', re.sub(r'(_\w+)=[1-9]', r'\1=0', sample))), name
            )

    def test_every_reference_resolves_here_or_names_its_branch(self):
        names = {c.name for group in checks.GROUPS.values() for c in group}
        for lane, entries in owed_all.LANES.items():
            for e in entries:
                if isinstance(e, owed.Ref):
                    self.assertTrue(e.name in names or e.lands, f'{lane}: {e.name}')

    def test_a_lane_fails_on_any_failure_passes_on_a_pass_and_names_what_is_not_here(self):
        def result(name: str, verdict: str) -> Result:
            return Result(Check(name, 'big-x', '', REPORT), verdict, '')

        entries = [Check('owed.x.a', 'big-x', 'a'), Check('owed.x.b', 'big-x', 'b'), owed.Ref('y.c', 'big/y')]
        by_check = {'owed.x.a': result('owed.x.a', PASS), 'owed.x.b': result('owed.x.b', INFO)}
        row = owed.lane_row('x', entries, by_check)
        self.assertEqual(row.verdict, PASS)
        self.assertEqual(row.observed, 'PASS 1, INFO 1; not on this branch: y.c (with big/y)')
        by_check['owed.x.b'] = result('owed.x.b', FAIL)
        self.assertEqual(owed.lane_row('x', entries, by_check).verdict, FAIL)
        self.assertIn(': owed.x.b', owed.lane_row('x', entries, by_check).observed)
        self.assertEqual(owed.lane_row('x', entries, {}).verdict, NO_DATA)
        rows = owed_all.summary({}, ['ABCD1234 2026-11-26 10:00:00 to 10:20:00'])
        self.assertEqual(rows[0].check.name, 'owed.archive')
        self.assertEqual(
            [r.check.name for r in rows[1:]], [f'owed.{lane}' for lane in owed_all.LANES] + ['owed.motion']
        )
        self.assertEqual(rows[-1].verdict, NA)
        self.assertTrue(rows[-1].observed.startswith('not in this image'))


if __name__ == '__main__':
    unittest.main()
