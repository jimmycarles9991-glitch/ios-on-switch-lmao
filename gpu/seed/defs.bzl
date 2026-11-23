"""The shader cache seed: what the host tools (tools.bzl) make from the IPSW's Metal libraries on the Mac that composes
the image.

  shader_crawl   the IPSW kind's crawled libraries (crawl.bzl crawls them once, in a repository) and their names
  metal_seed     resolve the corpus, plan the buckets, the native records of each (native.bzl), and the Metal compiler's
                 replies the device kept
  seed_pipeline_pack  the driver's pipeline pack, from Core Animation's corpus lines
  sample_pipeline_pack  a raw pack of some of seed_pipeline_pack's states, made as it makes them (the record goldens' test)
  seed_tree      the replies, the native records and the pack laid out as the Data volume holds them (the image
                 builder's)

Every output is named below the IPSW kind (21H461/, 24B119/): a build of the other kind writes its own files, so
switching --config=ios27 on and off reuses both instead of overwriting one with the other.
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//ipsw:ipsw.bzl", "IpswInfo")
load(":crawl.bzl", "QUARTZCORE")
load(":native.bzl", "DARWIN", "named", "native_pack", "native_records", "three")
load(":tools.bzl", "raw_tool_files", "seed_tool_files")

_IPSW = {"ipsw": attr.label(default = "//ipsw:current", providers = [IpswInfo])}

def _repo_root(files):
    return files[0].owner.workspace_root

CrawlInfo = provider(
    doc = "A crawl of the IPSW's images: the library tree (root/, data/) and the QuartzCore library in it.",
    fields = {
        "files": "depset: every file of the tree",
        "root": "the tree's directory, which holds root/ and data/",
        "qc": "QuartzCore's default.metallib",
        "base_files": "depset: the same tree without the applications' libraries (their own buckets read those)",
        "base_root": "the directory holding that tree's root/ and data/",
    },
)

SeedInfo = provider(
    doc = "The seed: the Metal compiler's replies and the native records, each as the device reads them.",
    fields = {
        "replies": "msl/ (the replies) and MANIFEST.json (their inventory and the IPSW build)",
        "native": "the native records as the device reads them, packed (native.bzl)",
    },
)

def _shader_crawl_impl(ctx):
    qc = ctx.file.qc
    if not qc.path.endswith("/" + QUARTZCORE):
        fail("qc must be the tree's " + QUARTZCORE)
    root = qc.path.removesuffix("/" + QUARTZCORE)
    crawled = root
    files = depset(ctx.files.tree)
    sources = []
    if ctx.files.applications or ctx.files.embedded:
        tree = ctx.actions.declare_directory(named(ctx, "_inputs"))
        report = ctx.actions.declare_file(named(ctx, "_inputs.json"))
        args = ctx.actions.args()
        args.add("--crawl", root)
        args.add_all(ctx.files.applications, before_each = "--application")
        args.add_all(ctx.files.embedded, before_each = "--embedded")
        args.add("--ios-major", ctx.attr.ipsw[IpswInfo].version.split(".")[0])
        args.add("--ipsw", ctx.attr.ipsw[IpswInfo].build)
        args.add("--out", tree.path)
        args.add("--report", report)
        ctx.actions.run(
            executable = ctx.executable._inputs,
            arguments = [args],
            inputs = depset(ctx.files.applications + ctx.files.embedded, transitive = [files]),
            outputs = [tree, report],
            mnemonic = "MetalSeedInputs",
            progress_message = "Adding this OS build's application and embedded shaders",
        )
        root = tree.path
        files = depset([tree, qc])
        sources = [report]
    base_files, base_root = files, root
    if ctx.files.applications:
        # Every seed bucket but the apps' own reads this: an HW Test app's shader edit reruns only the apps' buckets.
        base = ctx.actions.declare_directory(named(ctx, "_base"))
        args = ctx.actions.args()
        args.add("--crawl", crawled)
        args.add_all(ctx.files.embedded, before_each = "--embedded")
        args.add("--ios-major", ctx.attr.ipsw[IpswInfo].version.split(".")[0])
        args.add("--ipsw", ctx.attr.ipsw[IpswInfo].build)
        args.add("--out", base.path)
        ctx.actions.run(
            executable = ctx.executable._inputs,
            arguments = [args],
            inputs = depset(ctx.files.embedded, transitive = [depset(ctx.files.tree)]),
            outputs = [base],
            mnemonic = "MetalSeedBase",
            progress_message = "Adding the embedded shaders to the crawled libraries",
        )
        base_files, base_root = depset([base, qc]), base.path
    names = ctx.actions.declare_file(named(ctx, ".names.json"))
    ctx.actions.run(
        executable = ctx.executable._libraries,
        arguments = ["names", "--build", ctx.attr.ipsw[IpswInfo].build, root + "/root", names.path],
        inputs = files,
        outputs = [names],
        mnemonic = "M2vLibraryNames",
        progress_message = "Indexing the crawled libraries' function names",
    )
    return [
        DefaultInfo(files = depset([ctx.file.inventory] + sources, transitive = [files])),
        CrawlInfo(files = files, root = root, qc = qc, base_files = base_files, base_root = base_root),
        OutputGroupInfo(names = depset([names]), inventory = depset([ctx.file.inventory]), sources = depset(sources)),
    ]

shader_crawl = rule(
    implementation = _shader_crawl_impl,
    doc = "The IPSW kind's crawled Metal libraries (@ipsw//:crawl_*, crawl.bzl), and the names of their functions.",
    attrs = _IPSW | {
        "tree": attr.label(mandatory = True, allow_files = True, doc = "every file below the tree (root/, data/)"),
        "qc": attr.label(mandatory = True, allow_single_file = True, doc = "the tree's QuartzCore default.metallib"),
        "inventory": attr.label(mandatory = True, allow_single_file = [".json"], doc = "the crawl's inventory.json"),
        "applications": attr.label_list(allow_files = [".ipa"]),
        "embedded": attr.label_list(allow_files = [".h"]),
        "_inputs": attr.label(default = "//tools/gpu/shaders:inputs", executable = True, cfg = "exec"),
        "_libraries": attr.label(default = "//drivers/TegraGM20BMetal:libraries", executable = True, cfg = "exec"),
    },
)

def _plan(ctx, crawl, corpus):
    """The buckets' lines and the library views they read: the apps' buckets come last and alone read the apps'."""
    count, apps = ctx.attr.buckets, _app_buckets(ctx, crawl)
    input_reports = ctx.attr.crawl[OutputGroupInfo].sources.to_list()
    buckets = [
        ctx.actions.declare_file(named(ctx, "_plan/bucket-{}.log".format(three(i))))
        for i in range(count + apps)
    ]
    store = ctx.actions.declare_file(named(ctx, "_plan/store.json"))
    base_store = ctx.actions.declare_file(named(ctx, "_plan/store-base.json")) if apps else store
    report = ctx.actions.declare_file(named(ctx, "_plan/plan.json"))
    args = ctx.actions.args()
    args.add("--corpus", corpus)
    args.add("--root", crawl.root + "/root")
    args.add("--data-dir", crawl.root + "/data")
    args.add_all(ctx.files.device_libraries, before_each = "--data")
    args.add("--qc", crawl.qc)
    args.add("--qc-sha256", ctx.attr.ipsw[IpswInfo].derived["quartzcore_metallib_sha256"])
    args.add("--depth", ctx.attr.depth[BuildSettingInfo].value)
    args.add("--buckets", buckets[0].dirname)
    args.add("--count", count)
    args.add("--store", store)
    args.add("--report", report)
    args.add_all(input_reports, before_each = "--input-report")
    if apps:
        args.add("--apps", apps)
        args.add("--base-root", crawl.base_root + "/root")
        args.add("--base-data-dir", crawl.base_root + "/data")
        args.add("--base-store", base_store)
    ctx.actions.run(
        executable = ctx.executable._plan,
        arguments = [args],
        inputs = depset(
            [corpus] + ctx.files.device_libraries + input_reports,
            transitive = [crawl.files, crawl.base_files],
        ),
        outputs = buckets + [store, report] + ([base_store] if apps else []),
        mnemonic = "MetalSeedPlan",
        progress_message = "Planning the shader seed's buckets",
    )
    full = struct(root = crawl.root, files = crawl.files, store = store)
    base = struct(root = crawl.base_root, files = crawl.base_files, store = base_store)
    return buckets, [base] * count + [full] * apps, report

def _app_buckets(ctx, crawl):
    return ctx.attr.app_buckets if crawl.base_root != crawl.root else 0

def _metal_seed_impl(ctx):
    crawl = ctx.attr.crawl[CrawlInfo]
    names = ctx.attr.crawl[OutputGroupInfo].names.to_list()[0]
    tools = seed_tool_files(ctx.attr.tools)
    corpus = ctx.actions.declare_file(named(ctx, "_plan/corpus.log"))
    ctx.actions.run(
        executable = ctx.executable._libraries,
        arguments = ["resolve", names.path, ctx.file.corpus.path, corpus.path],
        inputs = [names, ctx.file.corpus],
        outputs = [corpus],
        mnemonic = "MetalSeedCorpus",
        progress_message = "Resolving the shader corpus's libraries",
    )
    lines, views, report = _plan(ctx, crawl, corpus)
    native_stamped, native_checked = native_records(ctx, tools, crawl, views, lines)

    replies = ctx.actions.declare_directory(named(ctx, "_replies"))
    args = ctx.actions.args()
    args.add("replies")
    if ctx.files.replies:
        args.add("--replies", _repo_root(ctx.files.replies) + "/msl")
        args.add_all(ctx.files.replies, before_each = "--reply-input")
    if ctx.attr._profile[BuildSettingInfo].value == "release":
        args.add("--strict-replies")
    args.add("--ipsw", ctx.attr.ipsw[IpswInfo].build)
    args.add("--out", replies.path)
    ctx.actions.run(
        executable = ctx.executable._merge,
        arguments = [args],
        inputs = ctx.files.replies,
        outputs = [replies],
        mnemonic = "MetalSeedReplies",
        progress_message = "Laying out the Metal compiler's replies",
    )
    native_packed = native_pack(ctx, native_stamped)

    # The tools must be the GPU bundle's, or no key of the seed would ever be hit: checked beside the build, which
    # neither waits for it nor reruns anything when only the bundle changes.
    checked = ctx.actions.declare_file(named(ctx, ".tools-match"))
    args = ctx.actions.args()
    args.add("check")
    args.add("--tools-manifest", tools.manifest)
    args.add_all(ctx.files.base, before_each = "--base", expand_directories = False)
    args.add("--out", checked)
    ctx.actions.run(
        executable = ctx.executable._merge,
        arguments = [args],
        inputs = [tools.manifest] + ctx.files.base,
        outputs = [checked],
        mnemonic = "MetalSeedCheck",
        progress_message = "Checking the shader tools are the GPU bundle's",
    )
    return [
        DefaultInfo(files = depset([replies, native_packed])),
        SeedInfo(replies = replies, native = native_packed),
        OutputGroupInfo(
            plan = depset([corpus, report] + lines + [v.store for v in views]),
            native = depset([native_stamped, native_checked]),
            _validation = depset([checked, native_checked]),
        ),
    ]

metal_seed = rule(
    implementation = _metal_seed_impl,
    doc = """The seed the device reads beside the plugin: the driver's native records of the corpus's states, and the
    Metal compiler's replies the device kept.""",
    attrs = _IPSW | {
        "corpus": attr.label(mandatory = True, allow_single_file = True),
        "crawl": attr.label(mandatory = True, providers = [CrawlInfo], doc = "the IPSW's libraries (shader_crawl)"),
        "device_libraries": attr.label(allow_files = True, doc = "libraries only the device made (@gpu_device_data)"),
        "replies": attr.label(allow_files = True, doc = "the device's Metal compiler replies (@gpu_device_data)"),
        "embedded": attr.label_list(allow_files = [".h"], doc = "C arrays of metallibs the libraries' store holds"),
        "tools": attr.label(mandatory = True, doc = "the host tools (seed_tools, or files with MANIFEST.json)"),
        "base": attr.label(mandatory = True, doc = "the GPU bundle the seed is for (//gpu:base or the release's)"),
        "depth": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "buckets": attr.int(default = 256, doc = "a corpus line costs its bucket's compile: a 256th of the seed"),
        "app_buckets": attr.int(default = 2, doc = "the buckets of the apps' own lines, after the others"),
        "_libraries": attr.label(default = "//drivers/TegraGM20BMetal:libraries", executable = True, cfg = "exec"),
        "_plan": attr.label(default = "//tools/gpu/shaders:plan", executable = True, cfg = "exec"),
        "_native_seed": attr.label(default = "//tools/gpu/shaders:native_seed", executable = True, cfg = "exec"),
        "_seedpack": attr.label(default = "//kits/SwitchSeedPack:seedpack", executable = True, cfg = "exec"),
        "_profile": attr.label(default = "//bazel:profile", providers = [BuildSettingInfo]),
        "_merge": attr.label(default = "//tools/gpu/shaders:merge", executable = True, cfg = "exec"),
    },
)

# make_pack.py's BUCKETS: the split writes this many bucket corpora, then our own apps' states in one more, each bucket
# made by its own action.
_METAL_BUCKETS = 16

# The generator's modules each step runs (make_pack.py imports or runs them by path, beside it). Python finds a module
# beside a script through the script's real path, so a module missing here still imported in the local sandbox (its
# inputs are links into the source tree) and failed on a remote worker.
_METAL_STAMP = ["make_pack.py", "packfile.py", "selector_section.py"]
_METAL_SPLIT = _METAL_STAMP + [
    "ca/caset.py",
    "ca/caspec.py",
    "ca/caspec27.py",
    "ca/deepcolour.py",
    "ca/fetchfree.py",
    "iterated/programs.py",
    "iterated/programs_long.py",
    "iterated/table.py",
    "iterated/ucode.py",
    "rb/rbfree.py",
    "rb/rbspec.py",
]

# A bucket runs directbucket.py alone (it imports none of the other modules), with directpack and the mock it loads;
# bucket 0 also draws the driver's own states (own.metallib) and keeps the context init nvdec ran (context.log).
_METAL_PROBE = ["directbucket.py"]
_PROBE_TOOLS = ["directpack", "libmock_iokit.dylib"]
_PROBE0_TOOLS = _PROBE_TOOLS + ["own.metallib", "context.log"]
_METAL_LINK = [
    "make_pack.py",
    "directlink.py",
    "fetchfree_link.py",
    "iterated/programs.py",
    "iterated/programs_long.py",
    "iterated/table.py",
    "iterated/ucode.py",
    "mkpack.py",
    "kernels.py",
    "layouts.py",
    "packfile.py",
    "probelog.py",
    "selector_section.py",
    "splits.py",
    "archives.py",
    "draw_context.txt",
    # Read beside the generator's directory: mkpack.py's owned methods, layouts.py's colour table, packfile.py's chip
    # facts and own states.
    "../owned.h",
    "../srgb_table.h",
    "../chip_facts.h",
    "../own_states.h",
]

def _generator_files(ctx, names):
    here = ctx.file.generator.dirname
    parent = here.rpartition("/")[0]
    wanted = [parent + "/" + n.removeprefix("../") if n.startswith("../") else here + "/" + n for n in names]
    files = [f for f in ctx.files.srcs if f.path in wanted]
    if len(files) != len(names):
        fail("the pack generator's srcs lack some of " + ", ".join(names))
    return files

def _pack_buckets(ctx, tools, corpora, views, selectors = []):
    """Probe each corpus over its view of the libraries (views[i]: a root and its files)."""
    generator = _generator_files(ctx, _METAL_PROBE)
    buckets = []
    for i, bucket_corpus in enumerate(corpora):
        view = views[i]

        # directbucket.py knows a bucket by its directory's name (bucket0 also draws the driver's own states).
        bucket = ctx.actions.declare_directory(named(ctx, "_work/bucket{}".format(i)))
        probe_tools = raw_tool_files(tools, _PROBE0_TOOLS if i == 0 else _PROBE_TOOLS)
        args = ctx.actions.args()
        args.add("--generator", generator[0])
        args.add("--raw", tools.raw_dir)
        args.add("--qc", view.qc)

        # The probe reads the libraries under --root, which `view` declares whole: a remote worker has only the declared
        # files.
        args.add("--data-dir", view.root + "/data")
        args.add("--root", view.root + "/root")
        args.add_all(ctx.files.device_libraries, before_each = "--data")
        args.add("--lines", bucket_corpus)
        args.add_all(selectors, before_each = "--ca-selectors")
        args.add("--out", bucket.path)
        ctx.actions.run(
            executable = ctx.executable._probe,
            arguments = [args],
            inputs = depset([bucket_corpus] + probe_tools + generator + selectors + ctx.files.device_libraries, transitive = [view.files]),
            outputs = [bucket],
            mnemonic = "MetalRawProbe",
            progress_message = "Making the pack's bucket {} of {} directly".format(i + 1, len(corpora)),
            execution_requirements = DARWIN,
        )
        buckets.append(bucket)
    return buckets

def _pack_link(ctx, buckets, corpus_report, archives, selectors = []):
    linked = ctx.actions.declare_directory(named(ctx, "_work/linked"))
    args = ctx.actions.args()
    args.add("link")
    args.add("--generator", ctx.file.generator)
    args.add("--out", linked.path)
    if corpus_report:
        args.add("--corpus-report", corpus_report)
    args.add_all(archives, before_each = "--archives")
    args.add_all(selectors, before_each = "--ca-selectors")
    args.add_all(buckets, expand_directories = False)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = depset(([corpus_report] if corpus_report else []) + archives + selectors + buckets +
                        _generator_files(ctx, _METAL_LINK)),
        outputs = [linked],
        mnemonic = "MetalRawLink",
        progress_message = "Linking the driver's pipeline pack",
    )
    return linked

def _seed_pipeline_pack_impl(ctx):
    tools = seed_tool_files(ctx.attr.tools)
    crawl = ctx.attr.crawl[CrawlInfo]
    qc = [crawl.qc]
    lines = ctx.actions.declare_file(named(ctx, "_work/corpus.log"))
    corpus_report = ctx.actions.declare_file(named(ctx, "_work/corpus-report.txt"))
    kind = ctx.attr.ipsw[IpswInfo].kind
    archives = [f for f in ctx.files.archives if f.basename == kind + ".tsv"]
    app_states = [f for f in ctx.files.app_states if f.basename == kind + ".log"]
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = ["corpus", "--corpus", ctx.file.corpus.path, "--out", lines.path, "--report", corpus_report.path] +
                    ["--build", ctx.attr.ipsw[IpswInfo].build] + ["--archives=" + f.path for f in archives] +
                    ["--app-states=" + f.path for f in app_states],
        inputs = [ctx.file.corpus] + archives + app_states,
        outputs = [lines, corpus_report],
        mnemonic = "MetalRawCorpus",
        progress_message = "Picking the pack's corpus lines",
    )

    # QuartzCore's tables (ca/qc-<build>.json) make Core Animation's states from their names; RenderBox's
    # (rb/rb-<build>.json) give its states their fetch-free forms.
    tables = [
        f
        for f in ctx.files.srcs
        if (f.dirname.endswith("/ca") or f.dirname.endswith("/rb")) and f.extension == "json"
    ]
    corpora = [ctx.actions.declare_file(named(ctx, "_work/split/bucket{}.log".format(i))) for i in range(_METAL_BUCKETS + 1)]
    args = ctx.actions.args()
    names = [f for f in ctx.files.ca_names if f.basename == kind + ".txt"]
    args.add("split")
    args.add("--generator", ctx.file.generator)
    args.add("--qc", crawl.qc)
    args.add("--corpus", lines)
    args.add_all(names, before_each = "--ca-names")
    args.add("--ca-preferences", ctx.file._ca_preferences)
    args.add_all("--out", corpora)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = depset([lines, ctx.file._ca_preferences] + qc + tables + names + _generator_files(ctx, _METAL_SPLIT)),
        outputs = corpora,
        mnemonic = "MetalRawSplit",
        progress_message = "Splitting the pack's states into buckets",
    )

    # The shared buckets read the crawl without this build's applications; the apps' bucket, the last, reads them too.
    base = struct(root = crawl.base_root, files = crawl.base_files, qc = crawl.qc)
    full = struct(root = crawl.root, files = crawl.files, qc = crawl.qc)
    selectors = [f for f in ctx.files.ca_selectors if f.basename == kind + ".txt"]
    buckets = _pack_buckets(ctx, tools, corpora, [base] * _METAL_BUCKETS + [full], selectors)
    linked = _pack_link(ctx, buckets, corpus_report, archives, selectors)

    out = ctx.actions.declare_directory(named(ctx, ""))
    args = ctx.actions.args()
    args.add("stamp")
    args.add("--generator", ctx.file.generator)
    args.add("--pack", linked.path + "/pack.bin")
    args.add("--manifest", tools.manifest)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [linked, tools.manifest] + _generator_files(ctx, _METAL_STAMP),
        outputs = [out],
        mnemonic = "MetalRawStamp",
        progress_message = "Stamping the pack with the tools' stack key",
    )
    return [
        DefaultInfo(files = depset([out])),
        # report: the link's pack and report, and each bucket's states (test/directcheck makes the pack's records from them)
        OutputGroupInfo(report = depset([linked] + corpora), buckets = depset(buckets)),
    ]

seed_pipeline_pack = rule(
    implementation = _seed_pipeline_pack_impl,
    doc = "The driver's pipeline pack: Core Animation's corpus states made directly per bucket, linked, stamped.",
    attrs = _IPSW | {
        "tools": attr.label(mandatory = True),
        "crawl": attr.label(mandatory = True, providers = [CrawlInfo]),
        "device_libraries": attr.label(allow_files = True, doc = "device-compiled libraries used by native corpus states"),
        "corpus": attr.label(mandatory = True, allow_single_file = True),
        "archives": attr.label_list(
            allow_files = [".tsv"],
            doc = "the archive inventories (precompile/archives/<build>.tsv): the IPSW kind's, where there is one",
        ),
        "ca_names": attr.label_list(
            allow_files = [".txt"],
            doc = "Core Animation states devices named missing (precompile/ca-names/<build>.txt): the IPSW kind's",
        ),
        "ca_selectors": attr.label_list(allow_files = [".txt"]),
        "app_states": attr.label_list(
            allow_files = [".log"],
            doc = "our own apps' states the pack holds (precompile/app-states/<build>.log): the IPSW kind's",
        ),
        "generator": attr.label(mandatory = True, allow_single_file = True, doc = "pack/make_pack.py"),
        "srcs": attr.label_list(allow_files = True, doc = "the generator's modules and QuartzCore tables (pack)"),
        # The Core Animation preferences the device runs with decide which colour precision its QuartzCore names
        # states with (pack/ca/deepcolour.py)
        "_ca_preferences": attr.label(
            default = "//drivers/TegraGM20BMetal:stage-tegra-gm20b-metal.py",
            allow_single_file = True,
        ),
        "_tool": attr.label(default = "//tools/gpu:make_pack", executable = True, cfg = "exec"),
        "_probe": attr.label(default = "//tools/gpu:probe", executable = True, cfg = "exec"),
    },
)

def _sample_pipeline_pack_impl(ctx):
    tools = seed_tool_files(ctx.attr.tools)
    crawl = ctx.attr.crawl[CrawlInfo]
    kind = ctx.attr.ipsw[IpswInfo].build
    selectors = [f for f in ctx.files.ca_selectors if f.basename == kind + ".txt"]
    buckets = _pack_buckets(ctx, tools, [ctx.file.corpus], [struct(root = crawl.root, files = crawl.files, qc = crawl.qc)], selectors)
    return [DefaultInfo(files = depset([_pack_link(ctx, buckets, None, [])]))]

sample_pipeline_pack = rule(
    implementation = _sample_pipeline_pack_impl,
    doc = """A raw pack of a sample of seed_pipeline_pack's states: one bucket of corpus lines as seed_pipeline_pack's split wrote them
    (the first bucket's warm-up line first), probed and linked as seed_pipeline_pack probes and links its buckets, with no
    archive inventory and no stamp. Its records are the full pack's records of those states.""",
    attrs = _IPSW | {
        "ca_selectors": attr.label(default = "//drivers/TegraGM20BMetal/precompile/ca-selectors:tables"),
        "tools": attr.label(mandatory = True),
        "crawl": attr.label(mandatory = True, providers = [CrawlInfo]),
        "device_libraries": attr.label(allow_files = True, doc = "device-compiled libraries used by native corpus states"),
        "corpus": attr.label(mandatory = True, allow_single_file = True, doc = "split corpus lines, bucket0's"),
        "generator": attr.label(mandatory = True, allow_single_file = True, doc = "pack/make_pack.py"),
        "srcs": attr.label_list(allow_files = True, doc = "the generator's modules (pack)"),
        "_tool": attr.label(default = "//tools/gpu:make_pack", executable = True, cfg = "exec"),
        "_probe": attr.label(default = "//tools/gpu:probe", executable = True, cfg = "exec"),
    },
)

def _seed_tree_impl(ctx):
    seed = ctx.attr.seed[SeedInfo]
    out = ctx.actions.declare_directory(named(ctx, ""))
    size = ctx.actions.declare_file(named(ctx, ".size.json"))
    args = ctx.actions.args()
    args.add("tree")
    args.add("--seed", seed.replies.path)
    args.add("--native", seed.native.path)
    if ctx.file.raw:
        args.add("--raw", ctx.file.raw.path)
    args.add("--out", out.path)
    args.add("--size", size)
    ctx.actions.run(
        executable = ctx.executable._merge,
        arguments = [args],
        inputs = [seed.replies, seed.native] + ctx.files.raw,
        outputs = [out, size],
        mnemonic = "MetalSeedTree",
        progress_message = "Laying out the shader seed for the Data volume",
    )
    return [DefaultInfo(files = depset([out])), OutputGroupInfo(size = depset([size]))]

seed_tree = rule(
    implementation = _seed_tree_impl,
    doc = "ngpu/addons/TegraGM20BMetal.seed/shared (the replies) and TegraGM20BMetal.pack, for the Data volume's root.",
    attrs = _IPSW | {
        "seed": attr.label(mandatory = True, providers = [SeedInfo]),
        "raw": attr.label(allow_single_file = True),
        "_merge": attr.label(default = "//tools/gpu/shaders:merge", executable = True, cfg = "exec"),
    },
)
