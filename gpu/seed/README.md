# Shader seed

Every Metal pipeline iOS makes on the Switch goes through m2v (Metal to SPIR-V) and NAK (SPIR-V to GM20B code). On
the console's CPU that work takes up most of a cold first boot. The seed does it ahead of time on the Mac and ships the
results in m2v's own cache format. The device reads the seed beneath every process's cache directory, so a pipeline
that's in the seed costs one file read.

```sh
bazel build //gpu/seed:tree                                   # the seed and the raw pack, as the Data volume holds them
bazel build //gpu/seed:tree_size                              # its size, for the image builder
bazel build //gpu/seed:seed --output_groups=verify            # the corpus replayed over the seed: every key a hit
bazel build //gpu/seed:tree --//gpu/seed:depth=corpus         # only what consoles were seen making
bazel build //gpu/seed:tools_archive                          # the host tools, as the release ships them (needs Xcode)
```

`--config=ios27` makes the seed for iPadOS 27 from its IPSW instead.

## What goes in

The crawl reads every Metal library on the IPSW's OS volume and cryptexes, including the ones embedded in the dyld
shared cache. Its inventory also lists what it leaves out (GPU binary archives and pipeline libraries, which the
frontend refuses) and anything that doesn't parse. It runs once per IPSW, in a repository (`crawl.bzl`): Bazel downloads
the OS image and cryptexes, crawls them, deletes them and keeps the crawl (about 233 MB) in its repository contents
cache. Building the seed never needs the images, so they stay off the Mac until an image build (`//install`) fetches
them. Editing `crawl.py` or `metallib.py` downloads and crawls again.

- `corpus`: every pipeline in `drivers/TegraGM20BMetal/precompile/corpus.log`, the journal harvested from real boots.
  That includes the dispatch shapes (D lines) compute states were seen taking: a compute kernel's GM20B code depends
  on its threadgroup size, which Metal only names at the dispatch.
- `wide`: adds every compute kernel in the IPSW, specialized the way its library's other kernels were, and each
  recorded render program on every target its fragment shader was seen drawing to.
- `deep` (the default): adds each recorded render program with one function constant moved to another value that
  constant was seen taking, and dispatches every compute state no D line names at the 2D shape Apple's documentation
  computes from `threadExecutionWidth` and `maxTotalThreadsPerThreadgroup`.

A guess nobody makes costs disk space and build time. m2v's keys cover everything a pipeline is made from, so a wrong
guess only misses on the device.

## Why it's built on the user's Mac

The seed is made from Apple's shaders, so it can't be in a release. The image builder makes it from the IPSW the user
downloads, using host tools from the release (`--//gpu/seed:tools`, `--//gpu/seed:base`). Running them needs only
macOS. The merge refuses tools whose NVK or stack key differs from the GPU bundle's, because their seed would never be
hit.

The work is split into shards by program, one action per shard, so it runs in parallel and a corpus edit only redoes
the shards it touches. A derived pipeline that crashes the tool is skipped and counted. One from the corpus fails the
build, because it would crash the device too.

NVK names its pipeline cache by a hash of its compiler's sources and flags
(`third_party/mesa/bazel/compiler_identity.py`) rather than by the dylib, and each shard is keyed on that hash. An NVK
edit outside the compiler (images, memory, command buffers) reruns no shard and keeps the device's own shader caches.
Bazel's action cache does this through the shard's unused inputs list, so it holds within one output base; a fresh
output base still misses the disk cache.

The raw pack is probed in buckets too, one action each. Only the last step, which stamps the stack key into the pack,
reads the key. The key changes only with what the raw path's records are made and read by (`//gpu:raw_stack`), so an
edit to the rest of the raw runtime leaves the pack and every record on the device as they are.
