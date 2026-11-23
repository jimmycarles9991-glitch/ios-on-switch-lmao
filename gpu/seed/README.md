# Shader seed

The driver compiles every Metal pipeline it hasn't seen before on the console's CPU, which takes up most of a cold
first boot. The seed makes those records ahead of time on the Mac from the IPSW's own Metal libraries, and the device
reads them before it compiles anything. It also carries the Metal compiler replies a console kept (Core Image's kernels
compiled from source), which the shader compiler reads before it asks the OS's compiler.

```sh
bazel build //gpu/seed:tree                                   # the replies, native records and raw pack, as the Data volume holds them
bazel build //gpu/seed:tree_size                              # its size, for the image builder
bazel build //gpu/seed:tree --//gpu/seed:depth=corpus         # only what consoles were seen making
bazel build //gpu/seed:tools_archive                          # the host tools, as the release ships them (needs Xcode)
```

`--config=ios27` makes the seed for iPadOS 27 from its IPSW.

## What goes in

The crawl reads every Metal library on the IPSW's OS volume and cryptexes, including the ones embedded in the dyld
shared cache. Its inventory also lists what it leaves out (GPU binary archives and pipeline libraries) and anything
that doesn't parse. It runs once per IPSW, in a repository (`crawl.bzl`): Bazel downloads the OS image and cryptexes,
crawls them, deletes them and keeps the crawl (about 233 MB) in its repository contents cache. Building the seed never
needs the images, so they stay off the Mac until an image build (`//install`) fetches them. Editing `crawl.py` or
`metallib.py` downloads and crawls again.

- `corpus`: every pipeline in `drivers/TegraGM20BMetal/precompile/corpus.log`, the journal harvested from real boots,
  including the dispatch shapes (D lines) compute states were seen taking. A kernel's GM20B code depends on its
  threadgroup size, which Metal only names at the dispatch.
- `wide`: adds each recorded render program on every target its fragment shader was seen drawing to.
- `deep` (the default): adds each recorded render program with one function constant moved to another value that
  constant was seen taking.

A guess no process makes costs disk space and build time. A record's key covers everything a pipeline is made from,
so a wrong guess only misses on the device.

## Why it's built on the user's Mac

The seed is made from Apple's shaders, so it can't ship in a release. The image builder makes it from the IPSW the
user downloads, using host tools from the release (`--//gpu/seed:tools`, `--//gpu/seed:base`). Running them needs only
macOS. The seed refuses tools whose stack key differs from the GPU bundle's, because their records would never be hit.

The corpus is split into buckets by program, one action each, so they run in parallel and a corpus edit only redoes
the buckets it touches. The pack is probed in buckets too. Only the last step, which stamps the stack key into the
pack and the native records, reads the key. The key changes only with what the driver's records are made and read by
(`//gpu:stack_key`), so an edit to the rest of the runtime leaves the pack and every record on the device as they
are.
