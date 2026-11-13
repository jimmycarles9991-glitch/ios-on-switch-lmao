"""The kext prelink of a fileset kernelcache (iPadOS 26/27) into our kernel: //translate/arm64e:prelink, one action.

The kernelcache's kexts are arm64e and stripped. They are translated in kernel mode, every chained fixup is written as
a plain pointer, and each Apple kernel address they use is named from the kernelcache's vtables (PAC-checked), GOT
order (iOS 17's import names), its vnode operation vectors and ipsw's signatures, then pinned_names.py's hand pins.
The prelink refuses an import that nothing names unless shims.py answers it.
"""

load("@xcode//:defs.bzl", "DEVELOPER_DIR")
load(":common.bzl", "XCODE_EXECUTION")
load(":kernel.bzl", "KernelObjectsInfo")

def _kernel_prelink_fileset_impl(ctx):
    stem = ctx.attr.out[:-len(".macho")]
    out = ctx.actions.declare_file(ctx.attr.out)
    report = ctx.actions.declare_file(stem + ".json")
    names = ctx.actions.declare_file(stem + ".names.json")
    kernel = ctx.attr.kernel[KernelObjectsInfo].objects[0]
    args = ctx.actions.args()
    args.add("--kernel", kernel)
    args.add("--kernelcache", ctx.file.kernelcache)
    args.add("--kernelcache-17", ctx.file.kernelcache_17)
    args.add("--arm64e", ctx.executable._arm64e)
    args.add("--ipsw", ctx.executable._ipsw)
    args.add("--symbolsets", ctx.file.symbolsets)
    xnu_signatures = [f for f in ctx.files.signatures if f.basename == "xnu.json"]
    if len(xnu_signatures) != 1:
        fail("signatures must hold exactly one xnu.json")
    args.add("--signatures", xnu_signatures[0])
    args.add("--slide=" + ctx.attr.slide)
    args.add("--out", out)
    args.add("--report", report)
    args.add("--names", names)
    ctx.actions.run(
        executable = ctx.executable._prelink,
        arguments = [args],
        inputs = [kernel, ctx.file.kernelcache, ctx.file.kernelcache_17, ctx.file.symbolsets] + ctx.files._signature_tree,
        tools = [ctx.executable._arm64e, ctx.executable._ipsw],
        outputs = [out, report, names],
        env = {"DEVELOPER_DIR": DEVELOPER_DIR},
        mnemonic = "KernelPrelinkFileset",
        progress_message = "Prelinking the fileset kernelcache's kexts into %{label}",
        execution_requirements = XCODE_EXECUTION,
    )
    return [DefaultInfo(files = depset([out])), OutputGroupInfo(report = depset([report, names]))]

kernel_prelink_fileset = rule(
    implementation = _kernel_prelink_fileset_impl,
    doc = "Our kernel with a fileset kernelcache's kexts prelinked (out); its report (.json) and kernel names (.names.json) in the report output group.",
    attrs = {
        "kernel": attr.label(mandatory = True, providers = [KernelObjectsInfo], doc = "the unprelinked kernel"),
        "kernelcache": attr.label(default = "//ipsw:kernelcache", allow_single_file = True),
        "kernelcache_17": attr.label(default = "//ipsw:kernelcache_21H461", allow_single_file = True),
        "symbolsets": attr.label(mandatory = True, allow_single_file = [".plist"], doc = "the kernel's symbolsets.plist"),
        "signatures": attr.label(mandatory = True, allow_files = True, doc = "ipsw's signatures of the kernelcache's Darwin version (<version>/xnu.json and kexts/)"),
        "out": attr.string(mandatory = True, doc = "the prelinked kernel's path in the package, ending in .macho"),
        "slide": attr.string(mandatory = True, doc = "the kexts' slide the bindings assume (kits/SwitchABI's tables)"),
        "_arm64e": attr.label(default = "//translate/arm64e", executable = True, cfg = "exec"),
        "_ipsw": attr.label(default = "@ipsw_cli//:ipsw", executable = True, allow_single_file = True, cfg = "exec"),
        "_prelink": attr.label(default = "//translate/arm64e:prelink", executable = True, cfg = "exec"),
        # ipsw reads every Darwin version's signatures below the directory that holds `signatures`. With 27.2's alone
        # it names half the functions and the prelink refuses its GOT bindings. Only a remote worker, whose inputs are
        # the declared files, showed it: in the sandbox, xnu.json's link led into the whole repository.
        "_signature_tree": attr.label(default = "@symbolicator//:kernel", allow_files = True),
    },
)
