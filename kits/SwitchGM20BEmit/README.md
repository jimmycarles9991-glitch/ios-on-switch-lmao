# SwitchGM20BEmit

GM20B push words, one set of functions for the kernel and user space: the push builder, the copy engine's rectangle
copy, 3D state (render targets, viewport, scissor, clears, blending), barriers and the draw macros. It sits on
`kits/SwitchGM20B:classes` and never on `:caps`, so an emitter cannot branch on what a format supports.

```sh
bazel test //kits/SwitchGM20BEmit/...
```

User space depends on `:emit` and `:macros_h`. Raw's host harnesses and the bench library link `:emit_ndebug` instead,
built with NDEBUG as the Metal plugin builds its C units: their addresses are host pointers, wider than the 40-bit
address fields the debug checks hold them to. Rules that take headers and include directories of their own (the plugin,
`ios_binary`) list `:emit_headers` and `:macros` and put `mesa/` and `gen/` on the include path ahead of Mesa's headers
directory, which has Mesa's own `nv_push.h`.

A kernel driver names `kits/SwitchGM20B` and `kits/SwitchGM20BEmit` in `hdr_deps`, adds `SWITCH_GM20B_EMIT_INCLUDES`,
and compiles `EMIT_SOURCES` with XNU's C headers (the kit includes `kern/assert.h`), so not in the nouveau units'
freestanding recipe (drivers/TegraGM20B builds them as `TegraGM20B_emit`). `test:SwitchGM20BEmitKernelCheck` builds the
kit with the kernel toolchain and is never linked.

## Taken from Mesa

- `mesa/nv_push.h` is Mesa's `nv_push.h`, adapted. It uses `nv_push_compat.h` instead of `util/macros.h` and nothing
  from libc the kernel lacks. `P_IMMD` takes the one-word form whenever the value fits, as `raw::Push::m` does, so the
  words don't change with the optimisation level. It also names the copy, inline-to-memory and host subchannels.
- `gen/nv_push_cl*.h` is Mesa's `class_parser.py` output, unchanged except that stdio and the method-dump declarations
  are dropped (`mesa/kernel_safe_push_header.py`). `mesa_gen/` holds the untouched output, for Mesa's own sources built
  on the host.
- `mesa/nouveau_copy_rect.c` is NVK's `nouveau_copy_rect` on GM20B's copy class only, writing into the caller's push.
  Its origin methods continue the block-size method run, which saves one header word per block-linear side.
- `mme/switch_gm20b_macros.c` builds the raw path's macro ABI v6 with Mesa's MME builder around NVK's pre-Pascal draw
  loops. A zero instance count skips the loop, because Fermi's MME tests a loop's condition at its end. The indirect
  macros free their base registers before the loop, because the builder reuses a live register once all seven are
  taken.
- Mesa's MME simulator and decoder (`emit_mme_test`) are used from Mesa's tree.

The emitters are C because the generated helpers use C designated initialisers. C++ code calls them through the
prototypes. Without LTO a C++ caller pays a call per sequence; release builds inline them.

## Context init

`context/context.tsv` holds the channel's initial 3D and compute state: one row per method, in the order the channel
takes them, each with its reason. `gen_context` expands it into `gen/SwitchGM20BContext.h`, one entry per method word,
uploading NVK's 31 macro programs (`context/macros.txt`) where their rows stand. A row whose word is an address names
its relocation and holds 0, and the channel opener supplies the address. The rows are NVK's queue init
(`nvk_push_draw_state_init`, `nvk_push_dispatch_state_init` and the heap state). `context_test` checks them word for
word against what NVK made on the pack generator's mock GM20B (`test/testdata/context_capture.txt`) and checks that each
relocation takes one address throughout. To change a value, edit its row and reason and update the capture, saying why
in the commit.

## Adding an emitter

Write it in C with `P_MTHD`/`P_IMMD` and the generated `P_NV<class>_*` helpers, give its header a word bound, and add
a test that compares its words with whatever the drivers emit for the same inputs. Use word-for-word comparison where
the header forms agree, and method-for-method (`SwitchGM20BPushDecode.h`) where they don't. When a driver moves onto
the emitter, its old sequence stays in the test as the reference (the words it wrote, or a replay through `raw::Push`
with `raw_gpu.cpp`'s constants), and the test checks that the driver's source now calls the kit.

Tests check every push they decode and every macro they run against what GM20B takes (`test/method_checks.h`). A method
its class lacks, or a shadow scratch word past the Fermi MME's 128, fails the test; on the GPU either one is an
ILLEGAL_MTHD that kills the channel. `method_ranges_test` checks the macro table against the Fermi MME's limits.
