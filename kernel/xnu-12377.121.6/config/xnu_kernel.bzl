"""The kernel Mach-O from XNU's native objects (xnu_objects.bzl), as XNU's config/ and makedefs/MakeInc.kernel make it:
version.c (newvers.pl), the KPI export lists and symbol sets, and the link (make's ld invocation, recipes_<config>.bzl
<CONFIG>_LINK, with the driver archives in LD_KERNEL_ARCHIVES' place).

The link names every object by the path make's link used (the debug map, N_OSO, is part of the kernel's bytes): the
objects are linked from a tree of links laid out as make's object root was, with -oso_prefix at its root.
"""

load("@xcode//:defs.bzl", "DEVELOPER_DIR", "IPHONEOS_SDK", "XCODE_TOOLCHAIN")
load("//kernel/rules:common.bzl", "XCODE_EXECUTION")
load("//kernel/rules:kernel.bzl", "KernelObjectsInfo")
load("//kernel/rules:layout.bzl", "old_output_spelling")
load(":recipes_development.bzl", "DEVELOPMENT_LINK")
load(":recipes_release.bzl", "RELEASE_LINK", "RELEASE_LINK_LTO")
load(":selected.bzl", "selected_only")
load(":xnu_objects.bzl", "XnuObjectsInfo", "by_profile")

# Per XNU config: (the LTO step, or None; the link). RELEASE is MakeInc.kernel's PRE_LTO pair: the bitcode objects and
# nonlto.o linked -r into one machine-code object (justlto.o), then the link of the machine-code objects and justlto.o.
LINKS = {
    "DEVELOPMENT": (None, DEVELOPMENT_LINK),
    "RELEASE": (RELEASE_LINK_LTO, RELEASE_LINK),
}

# The paths below the link tree's root, as make's (main's) kernel link named them.
_MAKE_TREE = "out/native-a57/root-v1/tree"
_MAKE_OBJROOT = _MAKE_TREE + "/build/xnu/ios-obj"
_MAKE_SDKROOT = _MAKE_TREE + "/.cache/fakeroot"

# A clean environment for XNU's tools: XNU's make sorted with the hardware-tested kernels' collation (en_IE.UTF-8).
def _env(locale = "C"):
    return {
        "DEVELOPER_DIR": DEVELOPER_DIR,
        "LANG": locale,
        "LC_ALL": locale,
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "TZ": "UTC",
        "ZERO_AR_DATE": "1",
    }

# --- version.c --------------------------------------------------------------------------------------------------
def _xnu_version_c_impl(ctx):
    out = ctx.actions.declare_file("obj/{}_ARM64_BCM2837/version.c".format(ctx.attr.config))
    ctx.actions.run_shell(
        inputs = [ctx.file.template, ctx.file.newvers],
        outputs = [out],
        arguments = [
            ctx.file.template.path,
            ctx.file.newvers.path,
            out.path,
            ctx.attr.darwin_version,
            ctx.attr.builder,
            ctx.attr.build_date,
            ctx.attr.objroot_name + "/{}_ARM64_BCM2837".format(ctx.attr.config),
            ctx.attr.config.lower(),
        ],
        # make: cp version.c.template version.c; newvers.pl version.c (OBJROOT/xnuVersion holds the Darwin version, and
        # newvers.pl runs whoami for the builder: both given here, so the version string is fixed).
        command = """set -e
work="$PWD/$3.work"
mkdir -p "$work/bin" "$work/obj"
printf '#!/bin/sh\\necho %s\\n' "$5" > "$work/bin/whoami"
chmod +x "$work/bin/whoami"
printf '%s\\n' "$4" > "$work/obj/xnuVersion"
cp "$1" "$3"
chmod u+w "$3"
PATH="$work/bin:$PATH" SRCROOT=/xnu OBJROOT="$work/obj" KERNEL_BUILD_DATE="$6" KERNEL_BUILD_OBJROOT="$7" \\
    CURRENT_KERNEL_CONFIG_LC="$8" /usr/bin/perl "$2" "$3" > /dev/null
""",
        env = _env(),
        mnemonic = "XnuVersion",
        progress_message = "Stamping XNU's version.c",
        execution_requirements = XCODE_EXECUTION,
    )
    return [DefaultInfo(files = depset([out]))]

xnu_version_c = rule(
    implementation = _xnu_version_c_impl,
    doc = "version.c (config/version.c.template stamped by config/newvers.pl), at make's path below obj/.",
    attrs = {
        "build_date": attr.string(default = "Thu  1 Jan 1970 00:00:00 UTC", doc = "//kernel/rules:build_date"),
        "builder": attr.string(default = "switch-ios", doc = "whoami's answer (//kernel/rules:builder)"),
        "config": attr.string(default = "DEVELOPMENT"),
        "darwin_version": attr.string(default = "25.5.0", doc = "RC_DARWIN_KERNEL_VERSION"),
        "newvers": attr.label(default = "newvers.pl", allow_single_file = True),
        "objroot_name": attr.string(default = "ios-obj", doc = "the object root's name in the version string"),
        "template": attr.label(default = "version.c.template", allow_single_file = True),
    },
)

# --- The KPI export lists and symbol sets ------------------------------------------------------------------------
# config/Makefile SYMBOL_COMPONENT_LIST (+ Kasan, Kcov for the plists), in make's order.
_SYMBOL_SETS = ["BSDKernel", "IOKit", "Libkern", "Mach", "MACFramework", "Unsupported", "Private"]

def _xnu_exports_impl(ctx):
    kpi = ctx.actions.declare_file(ctx.attr.name + "/all-kpi.exp")
    alias = ctx.actions.declare_file(ctx.attr.name + "/all-alias.exp")
    plist = ctx.actions.declare_file(ctx.attr.name + "/symbolsets.plist")
    files = {f.basename: f for f in ctx.files.srcs}
    arch = "arm64"

    def exports(symbolset):
        return [files[n] for n in [symbolset + ".exports", "{}.{}.exports".format(symbolset, arch)] if n in files]

    # EXPORTS_FILES: each set's .exports and .<arch>.exports, then Unused.exports; Kasan/Kcov disabled on DEVELOPMENT.
    all_exports = [f for s in _SYMBOL_SETS for f in exports(s)] + [files["Unused.exports"]]
    kasan = [files["Kasan_disabled.exports"]]
    kcov = [files["Kcov_disabled.exports"]]
    plists = {f.path.split("/PlugIns/")[-1].split(".kext/")[0]: f for f in ctx.files.plists}
    sets = [(s, plists[s], exports(s)) for s in _SYMBOL_SETS] + [("Kasan", plists["Kasan"], kasan), ("Kcov", plists["Kcov"], kcov)]
    script = ["set -e", 'work="$PWD/{}.work"'.format(plist.path), 'mkdir -p "$work"']
    script.append("sh {} {} {}".format(files["generate_linker_exports.sh"].path, kpi.path, " ".join([f.path for f in all_exports + kasan + kcov])))
    script.append("sh {} {} {}".format(files["generate_linker_aliases.sh"].path, alias.path, " ".join([f.path for f in all_exports + kasan + kcov])))
    parts = []
    for name, info, inputs in sets:
        # The kext's Info.plist stamped by newvers.pl, then its symbol set plist (config/Makefile kext_plist_template,
        # symbol_set_plist_template).
        stamped = '"$work/{}.Info.plist"'.format(name)
        part = '"$work/{}.symbolset.plist"'.format(name)
        script += [
            "cp {} {}".format(info.path, stamped),
            "chmod u+w " + stamped,
            'printf "%s\\n" "{}" > "$work/xnuVersion"'.format(ctx.attr.darwin_version),
            'SRCROOT=/xnu OBJROOT="$work" /usr/bin/perl {} {} > /dev/null'.format(files["newvers.pl"].path, stamped),
            "sh {} {} {} {}".format(files["generate_symbolset_plist.sh"].path, part, stamped, " ".join([f.path for f in inputs])),
            "/usr/bin/plutil -lint -s " + part,
        ]
        parts.append(part)
    script += [
        "sh {} {} {}".format(files["generate_combined_symbolsets_plist.sh"].path, plist.path, " ".join(parts)),
        "/usr/bin/plutil -convert binary1 -s " + plist.path,
    ]
    ctx.actions.run_shell(
        inputs = ctx.files.srcs + ctx.files.plists,
        outputs = [kpi, alias, plist],
        command = "\n".join(script),
        env = _env("en_IE.UTF-8"),
        mnemonic = "XnuSymbolSets",
        progress_message = "Making XNU's export lists and symbol sets",
        execution_requirements = XCODE_EXECUTION,
    )
    return [
        DefaultInfo(files = depset([kpi, alias, plist])),
        OutputGroupInfo(kpi = depset([kpi]), alias = depset([alias]), symbolsets = depset([plist])),
    ]

xnu_exports = rule(
    implementation = _xnu_exports_impl,
    doc = "all-kpi.exp, all-alias.exp (the link's alias list) and symbolsets.plist (__LINKINFO,__symbolsets).",
    attrs = {
        "darwin_version": attr.string(default = "25.5.0"),
        "plists": attr.label_list(allow_files = [".plist"], doc = "System.kext/PlugIns/<set>.kext/Info.plist"),
        "srcs": attr.label_list(allow_files = True, doc = "config/'s .exports and generate_*.sh, newvers.pl"),
    },
)

# --- The link ----------------------------------------------------------------------------------------------------
def _xnu_kernel_impl(ctx):
    config = ctx.attr.config
    out = ctx.actions.declare_file(ctx.attr.out or "build/xnu/mach.{}.bcm2837.unstripped.noctf".format(config.lower()))
    objdir = "{}/{}_ARM64_BCM2837".format(_MAKE_OBJROOT, config)
    farm = []  # (file, path below the link tree's root)
    filelist = []
    for target in ctx.attr.components:
        info = target[XnuObjectsInfo]
        for f, name in zip(info.objects, info.names):
            path = "{}/{}/{}/{}".format(objdir, info.component, config, name)
            farm.append((f, path))
            filelist.append(path)
    kernel = ctx.attr.kernel_objects[XnuObjectsInfo]
    for f, name in zip(kernel.objects, kernel.names):
        farm.append((f, objdir + "/" + name))
    farm.append((ctx.file.alias_list, objdir + "/all-alias.exp"))
    farm.append((ctx.file.kpi_list, objdir + "/all-kpi.exp"))
    farm.append((ctx.file.symbolsets, objdir + "/symbolsets.plist"))
    firehose = [f for f in ctx.files.sdkroot if f.basename == "libfirehose_kernel.a"]
    if len(firehose) != 1:
        fail("{}: no libfirehose_kernel.a in sdkroot".format(ctx.label))
    farm.append((firehose[0], _MAKE_SDKROOT + "/usr/local/lib/kernel/libfirehose_kernel.a"))

    # LD_KERNEL_ARCHIVES as the XNU build script assembled it: firehose, then the inputs (force_load or plain).
    archives = ["-L{root}/" + _MAKE_SDKROOT + "/usr/local/lib/kernel", "-lfirehose_kernel"]
    archive_paths = ["{root}/" + _MAKE_SDKROOT + "/usr/local/lib/kernel/libfirehose_kernel.a"]
    if len(ctx.attr.inputs) != len(ctx.attr.modes):
        fail("inputs and modes must pair up")
    files = []
    for target, mode in zip(ctx.attr.inputs, ctx.attr.modes):
        if KernelObjectsInfo in target:
            items = target[KernelObjectsInfo].objects
        else:
            items = target.files.to_list()
            only = [f for f in items if f.extension == "a"]
            if len(items) > 1 and len(only) == 1:
                items = only
        if len(items) != 1:
            fail("{}: link input {} must be one file (or have one .a)".format(ctx.label, target.label))

        # Named in the link tree as before the layout move (layout.bzl old_output_spelling): the debug map keeps its paths.
        at = old_output_spelling(items[0].short_path)
        farm.append((items[0], at))
        files.append(items[0])
        archive_paths.append("{root}/" + at)
        if mode == "force_load":
            archives.append("-Wl,-force_load,{root}/" + at)
        elif mode == "plain":
            archives.append("{root}/" + at)
        else:
            fail("{}: mode {} (force_load or plain)".format(ctx.label, mode))

    def expand(values):
        out = []
        for value in values:
            if value == "@KERNEL_ARCHIVES@":
                out += archives
                continue
            if value.startswith("{objdir}//mach."):
                value = "{out}"
            out.append(value.replace("{sdkroot}", "{root}/" + _MAKE_SDKROOT).replace("{objdir}", "{root}/" + objdir))
        return out

    lto, link = LINKS[config]
    argv = (expand(lto) if lto else []) + ["--xnu-link--"] + expand(link) + ["-Wl,-oso_prefix,{root}/"]

    manifest = ctx.actions.declare_file(ctx.attr.name + ".links")
    ctx.actions.write(manifest, "".join(["{}\t{}\n".format(f.path, p) for f, p in farm]))
    listing = ctx.actions.declare_file(ctx.attr.name + ".filelist")
    ctx.actions.write(listing, "".join(["{root}/" + p + "\n" for p in filelist]))

    kpi = "{root}/" + objdir + "/all-kpi.exp" if [a for a in link if a.startswith("-Wl,-exported_symbols_list,")] else ""
    arguments = ctx.actions.args()
    extras = " ".join([f.path for f in ctx.files.extra_exports])
    arguments.add_all([manifest, listing, out, objdir, " ".join(kernel.names), kpi, " ".join(archive_paths), extras])
    arguments.add_all(argv)
    ctx.actions.run_shell(
        inputs = depset([manifest, listing] + [f for f, _ in farm] + ctx.files.extra_exports + [ctx.file._xcode_identity]),
        outputs = [out],
        arguments = [arguments],
        # The link tree: make's object root (and SDK root, and the archives' paths) as links to the inputs, so the
        # debug map names each object as main's kernel did; -oso_prefix strips the tree's own location.
        command = """set -e
root="$PWD/$3.tree"
manifest="$1"; listing="$2"; out="$PWD/$3"; cwd="$root/$4"; own="$5"; kpi="${{6//\\{{root\\}}/$root}}"
archives="${{7//\\{{root\\}}/$root}}"; extras=""; for e in $8; do extras="$extras $PWD/$e"; done; shift 8
/usr/bin/perl -e 'use File::Path qw(make_path); my ($root, $here) = @ARGV[0, 1]; open(M, "<", $ARGV[2]) or die;
    while (<M>) {{ chomp; my ($src, $dst) = split /\\t/; my $to = "$root/$dst"; (my $dir = $to) =~ s,/[^/]*$,,;
    make_path($dir); symlink("$here/$src", $to) or die "$to: $!"; }}' "$root" "$PWD" "$manifest"
mkdir -p "$root/{sdk}/usr/local/lib/kernel/platform"
sed "s#{{root}}#$root#" "$listing" > "$cwd/link.filelist"
lto=(); args=(); seen=0
for a in "$@"; do
    a="${{a//\\{{root\\}}/$root}}"; a="${{a//\\{{out\\}}/$out}}"
    if [ "$a" = --xnu-link-- ]; then seen=1; elif [ $seen = 0 ]; then lto+=("$a"); else args+=("$a"); fi
done
cd "$cwd"
if [ ${{#lto[@]}} -gt 0 ]; then
    # MakeInc.kernel PRE_LTO: bitcode objects (the 0x0b17c0de wrapper) to ltolink.filelist, the rest to
    # nonltolink.filelist, the link's objects then the kernel's own in make's order; then the -r LTO step, which has no
    # deployment flags: make's `xcrun -sdk iphoneos clang++` gives it the iPhoneOS SDK's platform through SDKROOT.
    : > ltolink.filelist; : > nonltolink.filelist
    for o in $(cat link.filelist) $own; do
        if [ "$(/usr/bin/od -An -N4 -tx4 "$o" | tr -d ' ')" = 0b17c0de ]; then echo "$o" >> ltolink.filelist
        else echo "$o" >> nonltolink.filelist; fi
    done
    SDKROOT={iphoneos_sdk} {clangxx} "${{lto[@]}}"
fi
if [ -n "$kpi" ]; then
    # ld takes every name of -exported_symbols_list as -u, and XNU's Private exports also name Apple's own code this
    # tree has no source for (the DART, SART and UAT translators, the PPL IOMMU calls, IOUnifiedAddressTranslator), which
    # no DEVELOPMENT kernel defines either. The list keeps its comments, its patterns and the names the link's inputs
    # define (after the LTO step: justlto.o, the machine-code objects, the archives) or alias; kpi.dropped lists the
    # rest. Then extra_exports: names the kernel exports beyond XNU's lists, which ld requires to be defined.
    {{ [ -e justlto.o ] && echo justlto.o
       if [ -e nonltolink.filelist ]; then cat nonltolink.filelist; else cat link.filelist; printf '%s\\n' $own; fi
       printf '%s\\n' $archives; }} > kpi.inputs
    /usr/bin/xargs {nm} -g -U -j < kpi.inputs | /usr/bin/grep '^_' | /usr/bin/sort -u > kpi.defined
    /usr/bin/awk -v dropped=kpi.dropped 'FILENAME == ARGV[1] {{ d[$1] = 1; next }}
        FILENAME == ARGV[2] {{ if ($1 in d) d[$2] = 1; next }}
        /^[[:space:]]*#/ || /[*?[]/ || ($1 in d) {{ print; next }}
        NF {{ print $1 > dropped }}' kpi.defined "$cwd/all-alias.exp" "$kpi" > kpi.exp
    for e in $extras; do /usr/bin/grep -v '^#' "$e" >> kpi.exp; done
    rm "$kpi"; mv kpi.exp "$kpi"
fi
{clangxx} "${{args[@]}}"
if LC_ALL=C grep -qF "$PWD" "$out"; then echo "the kernel names the execution root $PWD" >&2; exit 1; fi
""".format(sdk = _MAKE_SDKROOT, clangxx = XCODE_TOOLCHAIN + "/clang++", iphoneos_sdk = IPHONEOS_SDK, nm = XCODE_TOOLCHAIN + "/nm"),
        env = _env(),
        mnemonic = "XnuKernelLink",
        progress_message = "Linking XNU's kernel ({} objects and the ios-on-switch-lmao archives)".format(len(filelist)),
        execution_requirements = XCODE_EXECUTION,
    )
    prelink, cuts, deferred = [], [], []
    for target in ctx.attr.inputs:
        if KernelObjectsInfo in target:
            prelink += target[KernelObjectsInfo].prelink
            cuts += target[KernelObjectsInfo].cuts
            deferred += target[KernelObjectsInfo].deferred
    return [
        DefaultInfo(files = depset([out])),
        KernelObjectsInfo(objects = [out], prelink = prelink, cuts = cuts, deferred = deferred),
    ]

XNU = "//kernel/xnu-12377.121.6/"

_COMPONENTS = [XNU + c + ":objects" for c in ["osfmk", "bsd", "libkern", "iokit", "pexpert", "libsa", "security", "san"]]

def _xnu_link_objects_impl(ctx):
    infos = [t[XnuObjectsInfo] for t in ctx.attr.components]
    symbols = [f for info in infos for f in info.symbols]

    # The per-object name lists by path, one per line: xnu_symbols.py reads them from this file, so its command line
    # stays short (and its key readable in aquery).
    listing = ctx.actions.declare_file(ctx.label.name + ".symbols")
    ctx.actions.write(listing, "".join([f.path + "\n" for f in symbols]))
    return [
        DefaultInfo(files = depset([f for info in infos for f in info.objects])),
        OutputGroupInfo(symbols = depset(symbols), symbols_list = depset([listing])),
    ]

def _xnu_object_names_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".txt")
    lines = []
    for target in ctx.attr.components:
        info = target[XnuObjectsInfo]
        lines += ["{} {}\n".format(info.component, name) for name in info.names if not name.startswith("switch/")]
    ctx.actions.write(out, "".join(lines))
    return [DefaultInfo(files = depset([out]))]

xnu_object_names = rule(
    implementation = _xnu_object_names_impl,
    doc = "\"<component> <object>\" per line, every component's objects in link order (objects_test compares them " +
          "with doconf's Makefiles).",
    attrs = {
        "components": attr.label_list(providers = [XnuObjectsInfo], default = _COMPONENTS),
    },
)

xnu_link_objects = rule(
    implementation = _xnu_link_objects_impl,
    doc = "The components' objects the kernel links (make's link.filelist), in link order, and (output group " +
          "symbols) each one's defined names: what the archives linked beside XNU namespace against " +
          "(//kernel:xnu_symbols).",
    attrs = {
        "components": attr.label_list(providers = [XnuObjectsInfo], default = _COMPONENTS),
    },
)

_xnu_kernel = rule(
    implementation = _xnu_kernel_impl,
    doc = "XNU's kernel (mach.<config>.bcm2837.unstripped.noctf) from its native objects and the ios-on-switch-lmao archives " +
          "(inputs, modes: force_load or plain, in LD_KERNEL_ARCHIVES order).",
    attrs = {
        "alias_list": attr.label(allow_single_file = True, default = XNU + "config:all_alias_exp"),
        "components": attr.label_list(
            providers = [XnuObjectsInfo],
            default = _COMPONENTS,
            doc = "the components, in make's link order (link.filelist)",
        ),
        "config": attr.string(default = "DEVELOPMENT"),
        "extra_exports": attr.label_list(
            allow_files = [".exports"],
            doc = "names (one per line, # comments) a RELEASE kernel exports beside XNU's KPI lists",
        ),
        "inputs": attr.label_list(allow_files = True),
        "kernel_objects": attr.label(providers = [XnuObjectsInfo], default = XNU + "config:objects"),
        "kpi_list": attr.label(allow_single_file = True, default = XNU + "config:all_kpi_exp"),
        "modes": attr.string_list(),
        "out": attr.string(doc = "the kernel's path in the package (default: make's, build/xnu/mach.<config>.bcm2837.unstripped.noctf)"),
        "sdkroot": attr.label(allow_files = True, default = "//kernel/xnu-12377.121.6:sdkroot_files"),
        "symbolsets": attr.label(allow_single_file = True, default = XNU + "config:symbolsets_plist"),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    },
)

def xnu_kernel(name, **kwargs):
    """The selected profile's kernel link when the selected XNU is 12377."""
    _xnu_kernel(
        name = name,
        config = by_profile("DEVELOPMENT", "RELEASE"),
        target_compatible_with = selected_only(),
        **kwargs
    )
