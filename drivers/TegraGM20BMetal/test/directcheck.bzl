"""Build the native record comparison and its corpus/pack runfiles."""

load("//gpu/seed:crawl.bzl", "QUARTZCORE")
load("//gpu/seed:defs.bzl", "CrawlInfo")
load("//gpu/seed:tools.bzl", "seed_tool_files")
load("//ipsw:ipsw.bzl", "IpswInfo")

def _crawl_root(crawl):
    """The short path of the directory holding the crawl's root/ and data/."""
    if crawl.root == crawl.qc.path.removesuffix("/" + QUARTZCORE):
        return crawl.qc.short_path.removesuffix("/" + QUARTZCORE)
    for f in crawl.files.to_list():
        if f.path == crawl.root:
            return f.short_path
    fail("the crawl's root is none of its files")

def _raw_directcheck_impl(ctx):
    tools = seed_tool_files(ctx.attr.tools)
    crawl = ctx.attr.crawl[CrawlInfo]
    out = ctx.actions.declare_directory(ctx.attr.name)
    args = ctx.actions.args()
    args.add("--xcode-env", ctx.file._xcode_env)
    args.add("--translate", ctx.file.translate)
    args.add("--compile", ctx.file.compile)
    args.add("--metal-abi", ctx.file.metal_abi)
    args.add("--raw", tools.raw_dir)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._build,
        arguments = [args],
        inputs = depset(
            ctx.files.srcs + tools.raw +
            [ctx.file.translate, ctx.file.compile, ctx.file.metal_abi, ctx.file._xcode_env, ctx.file._xcode_identity],
        ),
        outputs = [out],
        mnemonic = "Directcheck",
        progress_message = "Building directcheck",
        execution_requirements = {"requires-darwin": ""},
    )
    pack = ctx.files.pack
    corpora = [f for f in ctx.attr.pack[OutputGroupInfo].report.to_list() if f.extension == "log"]
    kind = ctx.attr.ipsw[IpswInfo].build
    selectors = [f for f in ctx.files.ca_selectors if f.basename == kind + ".txt"][0]
    inputs = ctx.actions.declare_file(ctx.attr.name + ".json")
    ctx.actions.write(inputs, json.encode({
        "tools": out.short_path,
        "pack": pack[0].short_path + "/pack.bin",
        "corpora": [f.short_path for f in corpora],
        "qc": crawl.qc.short_path,
        "lib_root": _crawl_root(crawl) + "/root",
        "selectors": selectors.short_path,
    }))
    runfiles = ctx.runfiles(files = [out, inputs, crawl.qc, selectors] + pack + corpora, transitive_files = crawl.files)
    return [
        DefaultInfo(files = depset([inputs]), runfiles = runfiles),
        OutputGroupInfo(native_tools = depset([out])),
    ]

raw_directcheck = rule(
    implementation = _raw_directcheck_impl,
    doc = "directcheck and its inputs, as one JSON file whose runfiles hold them.",
    attrs = {
        "ipsw": attr.label(default = "//ipsw:current", providers = [IpswInfo]),
        "ca_selectors": attr.label(default = "//drivers/TegraGM20BMetal/precompile/ca-selectors:tables"),
        "srcs": attr.label_list(allow_files = True, doc = "directcheck.mm and every source it compiles"),
        "translate": attr.label(mandatory = True, allow_single_file = True),
        "compile": attr.label(
            default = "//third_party/mesa/src/nouveau/switch_compile:host",
            allow_single_file = True,
            doc = "the stage compile's macOS object",
        ),
        "metal_abi": attr.label(mandatory = True, allow_single_file = True),
        "tools": attr.label(mandatory = True, doc = "the shader tools the pack was made with (//gpu/seed:tools)"),
        "crawl": attr.label(mandatory = True, providers = [CrawlInfo]),
        "pack": attr.label(mandatory = True, providers = [OutputGroupInfo], doc = "//gpu/seed:raw_pack"),
        "_build": attr.label(
            default = "//drivers/TegraGM20BMetal/test:directcheck_build",
            executable = True,
            cfg = "exec",
        ),
        "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    },
)
