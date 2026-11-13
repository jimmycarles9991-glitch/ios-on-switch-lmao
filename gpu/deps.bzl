"""The GPU bundle's external inputs (MODULE.bazel `gpu` extension).

@gpu_device_data  what only a Switch makes (precompile/harvest.py --pack): the driver records m2v wrote, the Metal
               libraries Core Image compiled there and the compiler's replies that made them (the frontend's msl/
               cache, which the seed carries so no process compiles Core Image's sources on first use), one tar
               pinned by sha256 (DEVICE_DATA), fetched into the repository
               cache once from ~/Library/Application Support/switch-ios/gpu-device-data/ (or
               $SWITCH_HOME/gpu-device-data/), where harvest.py --pack writes it; never from a checkout. Empty on a
               machine that has never harvested a console (CI, anyone else's Mac): the seed is made without it.
@gpu_llvm_as   bin/llvm-as of the LLVM 23.1.4 release.
"""

# @gpu_device_data: the tar harvest.py --pack wrote (<state dir>/gpu-device-data/<file>), by its sha256. Its records are of the
# NVK identities the device booted (vendor, device, driver version, pipelineCacheUUID); the seed takes its own NVK's.
# Without the tar the seed has no device records and lacks the corpus lines of Core Image's device-compiled kernels,
# which the device compiles on first use.
DEVICE_DATA = {
    "file": "m2v-device-15a8018125e4fec3.tar",
    "sha256": "15a8018125e4fec3be68057d5d7bac1963f71a4849b0168f0094e566e6108aa3",
}

def _run(rctx, args):
    result = rctx.execute(args, quiet = True)
    if result.return_code:
        fail("{} failed ({}):\n{}{}".format(" ".join([str(a) for a in args]), result.return_code, result.stdout, result.stderr))

# --- Cargo.lock reading (gpu/nvk/repos.bzl vendors bindgen's and cbindgen's crates with it) ----------------------------

def lock_packages(text):
    """The [[package]] tables of a Cargo.lock as dicts of their string keys."""
    packages, current = [], None
    for line in text.splitlines():
        line = line.strip()
        if line == "[[package]]":
            current = {}
            packages.append(current)
        elif line.startswith("[") and line != "[[package]]":
            current = None
        elif current != None and " = \"" in line:
            key, _, value = line.partition(" = ")
            current[key] = value.strip('"')
    return packages

# --- @gpu_llvm_as --------------------------------------------------------------------------------------------------
# llvm-as of LLVM 23.1.4, the version the bridge's AIR fixtures were assembled with (Homebrew's; the bitcode records
# the writer's version, and the fixtures are compiled into the plugin). Only bin/llvm-as of the release archive is
# extracted; the archive is the same download (and sha256) as toolchains_llvm's, so the repository cache holds it once.
LLVM_RELEASE = {
    "url": "https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/LLVM-23.1.4-macOS-ARM64.tar.xz",
    "sha256": "5d3179db39adeca5b17a34d82708fe976bc60e055b05fe557878fb3fb64d39ef",
    "prefix": "LLVM-23.1.4-macOS-ARM64",
}

def _llvm_tools_impl(rctx):
    archive = "llvm.tar.xz"
    rctx.download(url = LLVM_RELEASE["url"], output = archive, sha256 = LLVM_RELEASE["sha256"])
    _run(rctx, ["/usr/bin/tar", "-xJf", archive, "--strip-components", "1"] +
               [LLVM_RELEASE["prefix"] + "/bin/" + tool for tool in rctx.attr.tools])
    rctx.delete(archive)
    rctx.file("BUILD.bazel", 'package(default_visibility = ["//visibility:public"])\n\nexports_files({})\n'.format(
        repr(["bin/" + tool for tool in rctx.attr.tools]),
    ))

    # Pinned downloads only: the repository contents cache shares this across output bases and worktrees.
    return rctx.repo_metadata(reproducible = True)

llvm_tools = repository_rule(
    implementation = _llvm_tools_impl,
    attrs = {"tools": attr.string_list(mandatory = True)},
)

# --- @gpu_device_data ----------------------------------------------------------------------------------------------

def state_dir(rctx):
    """Where the tools keep what is not the repository's: ~/Library/Application Support/switch-ios, or $SWITCH_HOME."""
    return rctx.getenv("SWITCH_HOME") or rctx.getenv("HOME", "") + "/Library/Application Support/switch-ios"

def _device_data_impl(rctx):
    found = False
    if DEVICE_DATA:
        # Only a repository cache miss reads the file (the first fetch on a machine): from the tools' state directory,
        # where harvest.py --pack writes it, never from a checkout. The device's data is Apple-derived (Core Image's
        # compiled kernels, NVK's records of Apple's shaders), so it stays out of the repository, like the IPSW.
        where = state_dir(rctx) + "/gpu-device-data/" + DEVICE_DATA["file"]
        fetched = rctx.download_and_extract(
            url = "file://" + where.replace(" ", "%20"),
            sha256 = DEVICE_DATA["sha256"],
            type = "tar",
            allow_fail = True,
        )
        found = fetched.success
        if not found:
            rctx.watch(where)

            # buildifier: disable=print
            print("@gpu_device_data: {} is in neither the repository cache nor {}; the seed is made without a console's data".format(
                DEVICE_DATA["file"],
                where,
            ))
    rctx.file("BUILD.bazel", """\
package(default_visibility = ["//visibility:public"])

# {file} (sha256 {sha256}): precompile/harvest.py --pack of the device's m2v caches.
filegroup(
    name = "records",
    srcs = glob(["records/**"], allow_empty = True),
)

filegroup(
    name = "libraries",
    srcs = glob(["data/**"], allow_empty = True),
)

filegroup(
    name = "replies",
    srcs = glob(["msl/**"], allow_empty = True),
)
""".format(**(DEVICE_DATA or {"file": "no device data", "sha256": "none"})))

    # A download pinned by sha256 is shared across output bases by the repository contents cache; an empty one is not,
    # and watches the tar's path, so a tar that turns up later is fetched.
    return rctx.repo_metadata(reproducible = found)

device_data = repository_rule(implementation = _device_data_impl)

# --- The extension -------------------------------------------------------------------------------------------------

def _gpu_impl(module_ctx):
    llvm_tools(name = "gpu_llvm_as", tools = ["llvm-as"])
    device_data(name = "gpu_device_data")
    return module_ctx.extension_metadata(reproducible = True)

gpu = module_extension(implementation = _gpu_impl)
