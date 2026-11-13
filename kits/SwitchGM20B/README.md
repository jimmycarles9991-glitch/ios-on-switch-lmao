# SwitchGM20B

What the GM20B GPU is.

`:classes` is NVIDIA's class headers for the engines the channels bind: 3D `b197` with its texture and sampler header
layouts, compute `b1c0` with QMD 01_07, copy `b0b5`, 2D `902d`, inline-to-memory `a140` and host `b06f`. They come from
Mesa's vendored copy and are used in place, one umbrella header per engine. `SwitchGM20BTegra.h` adds the few methods
those headers lack (the tiled cache and pixel-ordered texture invalidate from deko3d, the host1x syncpoint methods from
nvgpu). These headers are the only place a method number should come from. A kernel driver names `kits/SwitchGM20B` in
`hdr_deps` and adds `SWITCH_GM20B_INCLUDES` to its flags, because NVIDIA's headers include `nvtypes.h` from Mesa's
headers directory and the kernel toolchain passes no include paths of its own.

`:caps` is one table of what the chip can do, generated from `caps/*.tsv` into `SwitchGM20BCaps.h` (C tables for the
kernel and user space) and `gm20b_caps.json`. The Metal answers and the planner are meant to read this table, so they
cannot disagree. Rows describe the hardware; `caps_report` lists where today's code answers differently. Differences
between iOS versions belong in `kits/SwitchABI/kinds/<build>`.

```sh
bazel test //kits/SwitchGM20B/...
bazel run //kits/SwitchGM20B:caps_report     # rows below chip grade, and where today's device answers disagree
```

## Adding a row

Every row carries a mechanism, a grade, a citation and a HW Test:

- mechanism: class and method names (`b197 SET_ANTI_ALIAS, SET_ANTI_ALIAS_SAMPLE_POSITIONS; tegra SWITCH_...`), checked
  against the headers. Use `hw: <text>` for what no method controls.
- grade: `source` (a header, Mesa, nvgpu or a document says so), `host` (a host test here exercises it) or `chip`
  (shown on this chip by a device run).
- citation: `path[:line]` in this repository or `ext: <source>`. `caps_report` checks that every cited file and line
  exists.
- hwtest: an id in `hw_test_app`, or `planned:<id>` for a probe still to be written.

A format is `chip` when raw's format tables (`kFormats`, `raw_zeta.h`) have it and the device's format checks
(`metal.formats`, `gpu.clear.formats`, `metal.compressed`) run raw over it; BC formats stay `source`, since raw never
reaches them. `planned:gpu.x1.<row>` stands in until a probe has a real id.

`formats.tsv` names the Mesa pipe format, and its colour/zeta target, texture header word 0 and support bits come from
NIL's `nil_formats.csv`, read through NIL's own generator. `methods.tsv` files every class method and QMD field under a
`features.tsv` row. `limits.tsv` says whether a device answer above (`max`) or below (`min`) the value over-claims.

`:methods` is `SwitchGM20BMethods.h`: every method each class takes on this chip, which host tests check pushes
against. NVIDIA's headers don't give array lengths, so `caps/gen_methods.py` uses Mesa's, except for the MME arrays. The
3D class runs the Fermi MME, which has 128 shadow scratch words and 128 macros (`limits.tsv`); Mesa generates Turing's
256.

## Tests

`driver_constants_test` reads the hand-written method numbers and fields out of the drivers' sources at build time and
checks each against the headers (`test/driver_constants.tsv`). A constant whose value comes from the headers (class
names, drf.h macros, or other such constants, with no number of its own) needs no row, and a row for one fails. Every
other constant needs a row: a new one without a row fails, as does a row that no longer matches. A `mismatch` row is a
driver bug; fixing the bug fails the test until the row says `nv`.
`caps_test` checks that the generator's output is stable and rejects a bad row.
