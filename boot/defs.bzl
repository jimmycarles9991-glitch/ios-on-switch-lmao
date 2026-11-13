"""//boot:package, the Hekate boot entry, one action per step. Each step reads only what it needs, so an edit reruns
only the steps it reaches:

    boot_args.py ............................ boot-args.txt
    overlay.py plan  (manifest) ............. overlay-plan.json
    adt_edit.py      (board ADT, Apple's tree of the IPSW's iPad, plan, iOS 27 cache list, the roles' trust modules)
                                         ...... ios.adt
    overlay.py trust (plan, Mach-Os) ........ overlay.trustcache
    trust.py         (IPSW, overlay, GPU, iOS 27 pretranslated) ... trustcache.bundle, all in one static module
    build_native_loader.py layout (kernel, ADT, bundle, seed, boot-args)
                                         ...... plan.json, the window's 1 MiB LZ4 blocks in 128 shards (by the MiB
                                                each starts in)
    lz4 -12, one action per shard ........... each shard's blocks compressed and checked (an unchanged shard is a
                                               cache hit: a driver edit recompresses the native kernel's few blocks,
                                               a boot-arg, ADT or trust cache edit the tail's)
    build_native_loader.py join ............. layout.h, runs.h, kernel-window.bin, constants.json
    clang -c, one per loader source ......... loader objects (the window's constants only for those including them)
    loader_link.py ........................... loader.uboot.bin
    boot_script.py ........................... the boot script (u-boot-script.txt with the window's constants)
    bl33.py patch (once) / stage ............. bl33.bin: U-Boot, the script, loader, exporter and DTB in their slots,
                                               the window
    overlay.py tree  (plan, overlay files) .. overlay/
    cp ...................................... ios/ (the directory //deploy:sd, //install:sd and //boot:snapshot take)

So an overlay plist edit reruns the overlay tree and the final copy; a driver edit reruns the kernel's steps from the
layout on (the shards it changed, four loader objects, the link, the boot script, the bl33 stage and the copy); a
built-in boot-arg edit reruns from the layout on, but compresses only the window's tail. The Python steps run on the
hermetic interpreter and need nothing of Xcode; the loader's compiles do (Xcode's clang, the declared host input).

Under --//install:prebuilt=release the release's loader takes the place of the compiles and the link, and bl33 stage
refuses it unless this window's constants.json (everything the loader compiles in) equals the one it was built for.
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//ipsw:defs.bzl", "PretranslatedPinsInfo")
load("//ipsw:ipsw.bzl", "ios_version", "kernel_virt_base", "os_volume_uuid")

# Xcode's tools: macOS only, here or on a Mac remote worker with the same Xcode (the key includes @xcode//:identity).
_XCODE_EXECUTION = {"requires-darwin": ""}

# The loader is freestanding AArch64 code entered by U-Boot's bootm with the MMU and caches off (boot/loader).
# PROBE_QUIET: the shared handoff probe draws nothing and skips its FDT CRC/walk unless it refuses (the loader checks
# the FDT itself); everything before the copy runs with the caches off.
LOADER_COPTS = [
    "--target=aarch64-none-elf",
    "-march=armv8-a",
    "-mstrict-align",
    "-mgeneral-regs-only",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-pic",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-DPROBE_QUIET=1",
]

# -v in bootargs.txt gives the text console in debug (the loader sets Video.v_display to text); release never does.
_VERBOSE_CONSOLE = {"debug": "-DSWITCH_VERBOSE_CONSOLE=1", "release": "-DSWITCH_VERBOSE_CONSOLE=0"}

# The shell that runs an Xcode tool: DEVELOPER_DIR from @xcode//:xcode.env (its first argument), then the tool.
_XCODE_SHELL = 'DEVELOPER_DIR="$(/usr/bin/sed -n "s/^DEVELOPER_DIR=//p" "$1")"; export DEVELOPER_DIR; shift; exec "$@"'

def _xcode(ctx, tool, arguments, inputs, outputs, mnemonic, message):
    ctx.actions.run_shell(
        command = _XCODE_SHELL,
        arguments = [ctx.file.xcode_env.path, tool] + arguments,
        inputs = depset(inputs + [ctx.file.xcode_env, ctx.file.xcode_identity]),
        outputs = outputs,
        env = {"LC_ALL": "C", "PATH": "/usr/bin:/bin", "TZ": "UTC"},
        mnemonic = mnemonic,
        progress_message = message,
        execution_requirements = _XCODE_EXECUTION,
    )

def _run(ctx, tool, arguments, inputs, outputs, mnemonic, message):
    ctx.actions.run(
        executable = tool,
        arguments = arguments,
        inputs = inputs,
        outputs = outputs,
        mnemonic = mnemonic,
        progress_message = message,
    )

# The kernel window's shards (tools/package/loader_abi.py WINDOW_SHARDS: the MiB of the window a block starts in).
_WINDOW_SHARDS = 128

# A shard's directory: its MiB, at least two digits (build_native_loader.py shard_of).
def _shard_name(i):
    return str(i) if i >= 10 else "0" + str(i)

# One shard: lz4 -12 over each of its blocks (none, one, or a few small ones), each checked by decoding it again.
_COMPRESS_SHARD = """set -e
lz4="$1"; in="$2"; out="$3"
for block in "$in"/*; do
    [ -f "$block" ] || continue
    name="${block##*/}"
    "$lz4" -12 -B6 --no-frame-crc -c -q "$block" > "$out/$name.lz4"
    "$lz4" -d -c -q "$out/$name.lz4" | /usr/bin/cmp -s - "$block" || { echo "lz4: $name does not decode to its block" >&2; exit 1; }
done
"""

def _one(target, what):
    files = target.files.to_list()
    if len(files) != 1:
        fail("{} {} must be one file, not {}".format(what, target.label, len(files)))
    return files[0]

def _compile_loader(ctx, out, profile, layout_h, runs_h, constants):
    """The loader for this window: a compile per source (only those including layout.h/runs.h see it), linked."""
    includes = []
    for marker in ctx.files.loader_include_markers:
        includes += ["-I", marker.dirname]
    windowed = {f.path: True for f in ctx.files.loader_window_srcs}
    objects = []
    for i, src in enumerate(ctx.files.loader_srcs):
        obj = out("loader/part-%d.o" % i)
        reads_window = src.path in windowed
        _xcode(
            ctx,
            "/usr/bin/clang",
            LOADER_COPTS + [_VERBOSE_CONSOLE[profile]] + (["-I", layout_h.dirname] if reads_window else []) + includes +
            ["-c", src.path, "-o", obj.path],
            [src] + ctx.files.loader_hdrs + ([layout_h, runs_h] if reads_window else []),
            [obj],
            "LoaderCompile",
            "Compiling the loader's %s" % src.short_path,
        )
        objects.append(obj)
    for src in ctx.files.loader_window_srcs:
        if src not in ctx.files.loader_srcs:
            fail("loader_window_srcs {} is not in loader_srcs".format(src.short_path))

    loader = out("loader.uboot.bin")
    args = ctx.actions.args()
    args.add("--ld-lld", ctx.file._ld_lld)
    args.add("--objcopy", ctx.file._objcopy)
    args.add("--readelf", ctx.file._readelf)
    args.add("--linker-script", ctx.file.linker_script)
    args.add("--constants", constants)
    args.add("--out", loader)
    args.add_all(objects)
    ctx.actions.run(
        executable = ctx.executable._loader_link,
        arguments = [args],
        inputs = objects + [ctx.file.linker_script, constants],
        tools = [ctx.file._ld_lld, ctx.file._objcopy, ctx.file._readelf],
        outputs = [loader],
        mnemonic = "LoaderLink",
        progress_message = "Linking the native loader",
    )
    return loader

def _boot_package_impl(ctx):
    name = ctx.attr.name

    def out(path):
        return ctx.actions.declare_file(name + "_work/" + path)

    prebuilt = bool(ctx.attr.prebuilt_loader)
    if prebuilt != bool(ctx.attr.prebuilt_loader_constants):
        fail("prebuilt_loader and prebuilt_loader_constants go together")
    if not prebuilt and not (ctx.file.xcode_env and ctx.file.xcode_identity):
        fail("compiling the loader needs xcode_env and xcode_identity")
    smp = ctx.attr.smp[BuildSettingInfo].value
    profile = "release" if prebuilt else ctx.attr._profile[BuildSettingInfo].value
    release = profile == "release"

    # --- boot-args, overlay plan, ADT ------------------------------------------------------------------------------
    boot_args = out("boot-args.txt")
    flags = (["--smp"] if smp else []) + (["--release"] if release else [])
    _run(ctx, ctx.executable._boot_args, [boot_args.path] + flags, [], [boot_args], "BootArgs", "Writing the boot-args")

    # Manifest source -> file: the Mach-Os the trust module may name, then every other overlay file.
    binaries = {source: _one(target, "overlay binary") for target, source in ctx.attr.overlay_binaries.items()}
    files = dict(binaries)
    for target, source in ctx.attr.overlay_data.items():
        files[source] = _one(target, "overlay file")

    # Source files the manifest names by their path in the kernel tree (the manifest's ../): no mapping to keep.
    parent = ctx.file.overlay_manifest.short_path.rpartition("/")[0].rpartition("/")[0]
    tree = parent + "/" if parent else ""
    for f in ctx.files.overlay_files:
        if not f.short_path.startswith(tree):
            fail("overlay_files {} is not in the kernel tree {}".format(f.short_path, tree))
        files[f.short_path[len(tree):]] = f

    plan = out("overlay-plan.json")
    args = ctx.actions.args()
    args.add("plan")
    args.add("--manifest", ctx.file.overlay_manifest)
    args.add("--portrait-header", ctx.file.portrait_header)
    args.add("--kind", ctx.attr._kind[BuildSettingInfo].value)
    args.add_all(sorted(files.keys()), before_each = "--given")
    if release:
        args.add("--release")
    args.add("--out", plan)
    _run(ctx, ctx.executable._overlay, [args], [ctx.file.overlay_manifest, ctx.file.portrait_header], [plan], "OverlayPlan", "Planning the userland overlay")

    adt = out("ios.adt")
    args = ctx.actions.args()
    args.add("--adt", ctx.file.adt)
    args.add("--apple-adt", ctx.file._apple_adt)
    args.add("--overlay-plan", plan)
    args.add("--bt-patch", ctx.file.bt_patch)
    args.add("--portrait-header", ctx.file.portrait_header)
    args.add("--root-volume-uuid", os_volume_uuid(ctx.attr._kind[BuildSettingInfo].value))
    if smp:
        args.add("--smp")

    # iOS 27's Image4 will not start without the current boot's manifest hash; 17's AppleImage4 boots without one.
    manifest_hash = ios_version(ctx.attr._kind[BuildSettingInfo].value) >= 27
    if manifest_hash:
        args.add("--boot-manifest-hash")
    pretranslated = ctx.attr.pretranslated[PretranslatedPinsInfo]
    adt_inputs = [ctx.file.adt, ctx.file._apple_adt, plan, ctx.file.bt_patch, ctx.file.portrait_header]
    if pretranslated.cache_list:
        args.add("--a64x-cache-pretranslated", pretranslated.cache_list)
        adt_inputs.append(pretranslated.cache_list)

    # Each role's CDHashes, which the kernel authenticates the role's files by: the cryptexes' (Apple's trust cache and,
    # on iOS 27, its pretranslated module) and the pretranslated tree's.
    roles = {
        "os-cryptex": [ctx.file.system_trust] + [f for f in pretranslated.trust if f.basename == "system_cryptex.trustcache"],
        "app-cryptex": [ctx.file.app_trust] + [f for f in pretranslated.trust if f.basename == "app_cryptex.trustcache"],
        "pretranslated": [f for f in pretranslated.trust if f.basename == "os.trustcache"],
    }
    for role, modules in roles.items():
        args.add_all(modules, before_each = "--cdhashes", format_each = role + "=%s")
        adt_inputs += modules
    args.add("--out", adt)
    _run(ctx, ctx.executable._adt_edit, [args], adt_inputs, [adt], "BootAdt", "Giving the ADT its iPad identity")

    # --- trust caches ---------------------------------------------------------------------------------------------
    overlay_trust = out("overlay.trustcache")
    args = ctx.actions.args()
    args.add("trust")
    args.add("--plan", plan)
    args.add_all(["%s=%s" % (source, f.path) for source, f in sorted(binaries.items())], before_each = "--file")
    args.add("--out", overlay_trust)
    _run(ctx, ctx.executable._overlay, [args], [plan] + binaries.values(), [overlay_trust], "OverlayTrust", "Hashing the overlay's code")

    gpu = ctx.attr._gpu_trustcache[DefaultInfo].files.to_list()
    if len(gpu) > 1:
        fail("the GPU trust cache must be one file")
    bundle = out("trustcache.bundle")
    modules = [ctx.file.system_trust, ctx.file.app_trust, overlay_trust] + gpu + pretranslated.trust
    args = ctx.actions.args()
    args.add("--static", ctx.file.static_trust)
    args.add_all(modules, before_each = "--module")
    args.add("--out", bundle)
    _run(ctx, ctx.executable._trust, [args], [ctx.file.static_trust] + modules, [bundle], "BootTrust", "Merging the trust caches into the static module")

    # --- the kernel window and the loader --------------------------------------------------------------------------
    # The prelinked kernel as linked: Xcode's strip and llvm-objcopy both refuse its layout ("string table not at the
    # end of the file"), so a release kernel is stripped where it is linked, before the prelinker (which binds the kexts
    # to its global symbols, all a strip -x keeps).
    kernel = ctx.file.kernel

    # Layout and checks, then each shard of the window's LZ4 blocks compressed on its own (only the shards an edit
    # changed rerun), then the staged window and the loader's headers.
    window_plan = out("window/plan.json")
    shards = [ctx.actions.declare_directory(name + "_work/window/blocks/" + _shard_name(i)) for i in range(_WINDOW_SHARDS)]
    args = ctx.actions.args()
    args.add("layout")
    args.add("--kernel", kernel)
    args.add("--adt", adt)
    args.add("--trustcache", bundle)
    args.add("--data-seed", ctx.file.data_seed)
    args.add("--boot-args", boot_args)
    args.add("--plan", window_plan)
    args.add("--blocks", shards[0].dirname)
    args.add("--kind", ctx.attr._kind[BuildSettingInfo].value)
    args.add("--virt-base", kernel_virt_base(ctx.attr._kind[BuildSettingInfo].value))
    args.add("--root-volume-uuid", os_volume_uuid(ctx.attr._kind[BuildSettingInfo].value))
    if manifest_hash:
        args.add("--boot-manifest-hash")
    _run(ctx, ctx.executable._window, [args], [kernel, adt, bundle, ctx.file.data_seed, boot_args], [window_plan] + shards, "KernelLayout", "Laying out the kernel window")

    compressed = []
    for i, shard in enumerate(shards):
        lz4 = ctx.actions.declare_directory(name + "_work/window/lz4/" + _shard_name(i))
        ctx.actions.run_shell(
            command = _COMPRESS_SHARD,
            arguments = [ctx.file.lz4.path, shard.path, lz4.path],
            inputs = [shard],
            tools = [ctx.file.lz4],
            outputs = [lz4],
            env = {"LC_ALL": "C", "PATH": "/usr/bin:/bin"},
            mnemonic = "KernelCompress",
            progress_message = "Compressing the kernel window's MiB %d" % i,
        )
        compressed.append(lz4)

    layout_h, runs_h, window, constants = out("gen/layout.h"), out("gen/runs.h"), out("kernel-window.bin"), out("constants.json")
    args = ctx.actions.args()
    args.add("join")
    args.add("--plan", window_plan)
    args.add("--compressed", compressed[0].dirname)
    args.add("--layout-h", layout_h)
    args.add("--runs-h", runs_h)
    args.add("--window", window)
    args.add("--constants", constants)
    _run(ctx, ctx.executable._window, [args], [window_plan] + compressed, [layout_h, runs_h, window, constants], "KernelWindow", "Staging the kernel window")

    if prebuilt:
        loader, loader_constants = ctx.file.prebuilt_loader, [ctx.file.prebuilt_loader_constants]
    else:
        loader, loader_constants = _compile_loader(ctx, out, profile, layout_h, runs_h, constants), []

    # --- U-Boot and its script ------------------------------------------------------------------------------------
    script_text, script = out("boot-script.txt"), out("boot-script.img")
    args = ctx.actions.args()
    args.add("--template", ctx.file.script_template)
    args.add("--constants", constants)
    args.add("--dtb", ctx.file.dtb)
    args.add("--text", script_text)
    args.add("--image", script)
    _run(ctx, ctx.executable._boot_script, [args], [ctx.file.script_template, constants, ctx.file.dtb], [script_text, script], "BootScript", "Writing U-Boot's boot script")

    u_boot = out("u-boot.bin")
    _run(ctx, ctx.executable._bl33, ["patch", ctx.file.bl33.path, u_boot.path], [ctx.file.bl33], [u_boot], "BootUBoot", "Patching U-Boot's default environment")
    bl33 = out("bl33.bin")
    args = ctx.actions.args()
    args.add("stage")
    args.add("--u-boot", u_boot)
    args.add("--window", window)
    args.add("--constants", constants)
    args.add("--script", script)
    args.add("--loader", loader)
    args.add("--exporter", ctx.file.exporter)
    args.add("--dtb", ctx.file.dtb)
    args.add_all(loader_constants, before_each = "--loader-constants")
    args.add("--out", bl33)
    _run(ctx, ctx.executable._bl33, [args], [u_boot, window, constants, script, loader, ctx.file.exporter, ctx.file.dtb] + loader_constants, [bl33], "BootBl33", "Staging the boot files and the kernel window in bl33.bin")

    # --- the overlay and the directory -----------------------------------------------------------------------------
    overlay = ctx.actions.declare_directory(name + "_work/overlay")
    args = ctx.actions.args()
    args.add("tree")
    args.add("--plan", plan)
    args.add_all(["%s=%s" % (source, f.path) for source, f in sorted(files.items())], before_each = "--file")
    args.add("--out", overlay.path)
    _run(ctx, ctx.executable._overlay, [args], [plan] + files.values(), [overlay], "OverlayTree", "Laying out the userland overlay")

    switchroot = {
        "bl31.bin": ctx.file.bl31,
        "bl33.bin": bl33,
        "bootlogo.bmp": ctx.file.bootlogo,
    }
    package = ctx.actions.declare_directory(ctx.attr.out)
    commands = [
        'set -e; out="$1"',
        '/bin/mkdir -p "$out/switchroot/ios" "$out/bootloader/ini" "$out/bootloader/res"',
        '/bin/cp -R -L "%s" "$out/overlay"' % overlay.path,
        '/bin/cp -L "%s" "$out/bootloader/ini/ios.ini"' % ctx.file.ini.path,
        '/bin/cp -L "%s" "$out/bootloader/res/icon_ios_hue.bmp"' % ctx.file.icon.path,
    ] + [
        '/bin/cp -L "%s" "$out/switchroot/ios/%s"' % (f.path, n)
        for n, f in sorted(switchroot.items())
    ]
    ctx.actions.run_shell(
        command = "\n".join(commands),
        arguments = [package.path],
        inputs = [overlay, ctx.file.ini, ctx.file.icon] + switchroot.values(),
        outputs = [package],
        mnemonic = "BootPackage",
        progress_message = "Assembling the iOS boot entry",
    )
    return [
        DefaultInfo(files = depset([package])),
        OutputGroupInfo(
            adt = depset([adt]),
            boot_script = depset([script_text]),
            constants = depset([constants]),
            kernel_window = depset([window, layout_h, runs_h, constants, window_plan]),
            loader = depset([loader]),
            overlay = depset([plan, overlay_trust, overlay]),
            trust = depset([bundle]),
        ),
    ]

def _tool(label):
    return attr.label(default = label, executable = True, cfg = "exec")

boot_package = rule(
    implementation = _boot_package_impl,
    doc = "The Hekate boot entry (switchroot/ios/*, bootloader/ini/ios.ini, bootloader/res/icon_ios_hue.bmp, " +
          "overlay/<absolute path>) from the prelinked kernel, the IPSW's trust caches, the GPU trust cache, the " +
          "overlay, the boot logo and the native loader's sources.",
    attrs = {
        "out": attr.string(default = "ios"),
        "ini": attr.label(mandatory = True, allow_single_file = True, doc = "bootloader/ini/ios.ini"),
        "kernel": attr.label(mandatory = True, allow_single_file = True),
        "adt": attr.label(mandatory = True, allow_single_file = True, doc = "the board ADT (//boot:adt)"),
        "_apple_adt": attr.label(default = "//ipsw:device_tree", allow_single_file = True),
        "static_trust": attr.label(mandatory = True, allow_single_file = True, doc = "the StaticTrustCache module"),
        "app_trust": attr.label(mandatory = True, allow_single_file = True, doc = "the App cryptex's module"),
        "system_trust": attr.label(mandatory = True, allow_single_file = True, doc = "the System cryptex's module"),
        "data_seed": attr.label(mandatory = True, allow_single_file = True),
        "script_template": attr.label(mandatory = True, allow_single_file = True),
        "bl31": attr.label(mandatory = True, allow_single_file = True),
        "bl33": attr.label(mandatory = True, allow_single_file = True),
        "dtb": attr.label(mandatory = True, allow_single_file = True),
        "bootlogo": attr.label(mandatory = True, allow_single_file = True),
        "icon": attr.label(mandatory = True, allow_single_file = True),
        "exporter": attr.label(mandatory = True, allow_single_file = True),
        "bt_patch": attr.label(mandatory = True, allow_single_file = True),
        "overlay_manifest": attr.label(mandatory = True, allow_single_file = True),
        "portrait_header": attr.label(mandatory = True, allow_single_file = True),
        "overlay_binaries": attr.label_keyed_string_dict(allow_files = True, doc = "built Mach-O -> its manifest source"),
        "overlay_data": attr.label_keyed_string_dict(allow_files = True, doc = "other overlay file -> its manifest source"),
        "overlay_files": attr.label_list(allow_files = True, doc = "overlay files the manifest names by tree path"),
        "loader_srcs": attr.label_list(allow_files = [".c", ".S"], mandatory = True, doc = "in link order"),
        "loader_window_srcs": attr.label_list(allow_files = True, doc = "those of loader_srcs that include layout.h/runs.h"),
        "loader_hdrs": attr.label_list(allow_files = True),
        "loader_include_markers": attr.label_list(allow_files = True, doc = "one file per -I directory, in order"),
        "linker_script": attr.label(mandatory = True, allow_single_file = True),
        "prebuilt_loader": attr.label(
            allow_single_file = True,
            doc = "the release's loader.uboot.bin (--//install:prebuilt=release), staged instead of compiling one; " +
                  "the release's parts are release builds, so the package is composed as release whatever " +
                  "//bazel:profile is",
        ),
        "prebuilt_loader_constants": attr.label(
            allow_single_file = True,
            doc = "the constants.json it was built for: bl33 stage refuses a window whose constants differ",
        ),
        "lz4": attr.label(
            default = "//tools:lz4",
            cfg = "exec",
            allow_single_file = True,
            doc = "the kernel window's compressor (lz4 -12, one action per shard): //tools:lz4 or the release's build of it",
        ),
        "smp": attr.label(default = "//boot:smp", providers = [BuildSettingInfo]),
        "pretranslated": attr.label(
            default = "//ipsw:pretranslated",
            providers = [PretranslatedPinsInfo],
            doc = "iOS 27's pinned pretranslate run: its trust modules join the bundle, its cache list the ADT",
        ),
        "_gpu_trustcache": attr.label(default = "//boot:gpu_trustcache"),
        "_kind": attr.label(default = "//ipsw:kind"),
        "_profile": attr.label(default = "//bazel:profile"),
        # Only the loader's compile runs Xcode: a package staging the release's loader names neither, so the image
        # builder's graph on a Mac without Xcode holds none of @xcode's stand-ins.
        "xcode_env": attr.label(allow_single_file = True, doc = "@xcode//:xcode.env, unless the loader is prebuilt"),
        "xcode_identity": attr.label(allow_single_file = True, doc = "@xcode//:identity, unless the loader is prebuilt"),
        "_boot_args": _tool("//tools/package:boot_args"),
        "_overlay": _tool("//tools/package:overlay"),
        "_adt_edit": _tool("//tools/package:adt_edit"),
        "_trust": _tool("//tools/package:trust"),
        "_window": _tool("//tools/package:window"),
        "_loader_link": _tool("//tools/package:loader_link"),
        "_boot_script": _tool("//tools/package:boot_script"),
        "_bl33": _tool("//tools/package:bl33"),
        # LLVM 23.1.4's linker and binutils.
        "_ld_lld": attr.label(default = "@llvm//:bin/ld.lld", cfg = "exec", allow_single_file = True),
        "_objcopy": attr.label(default = "@llvm//:bin/llvm-objcopy", cfg = "exec", allow_single_file = True),
        "_readelf": attr.label(default = "@llvm//:bin/llvm-readelf", cfg = "exec", allow_single_file = True),
    },
)
