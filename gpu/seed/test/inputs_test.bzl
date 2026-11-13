"""Analysis tests of what the seed's actions are keyed on, over stand-in inputs (no IPSW is fetched):

- the crawl's names read the crawled tree and the tool alone;
- a bucket's compile reads its bucket's lines, the libraries and the three tools that make code: not the corpus, the
  GPU bundle, the tools' manifest or the raw pack's tools, so a corpus edit reaches only the buckets it rewrites and a
  plugin edit reaches none; it reports the NVK twin unused, keyed on the twin's compiler identity instead;
- the join reads the buckets' indexes and reports, never their packs or the tools;
- the bundle check is a validation action, so the GPU bundle is no input of anything that makes the seed;
- the raw pack's probes read their bucket's states, the probe tools, the twin and QuartzCore's library, and only the
  stamp reads the tools' manifest (the stack key).
"""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

def _actions(env, mnemonic):
    return [a for a in analysistest.target_actions(env) if a.mnemonic == mnemonic]

def _names(action):
    return [f.short_path for f in action.inputs.to_list()]

def _ours(names):
    return [p for p in names if not p.startswith("../")]

def _crawl_impl(ctx):
    env = analysistest.begin(ctx)
    (names,) = _actions(env, "M2vLibraryNames")
    stray = [p for p in _ours(_names(names)) if "/crawl/tree/" not in p and not p.startswith("drivers/")]
    asserts.equals(env, [], stray, "the names' inputs beyond the tree and the tool")
    return analysistest.end(env)

_crawl_test = analysistest.make(_crawl_impl)

def _seed_impl(ctx):
    env = analysistest.begin(ctx)
    buckets = _actions(env, "MetalSeedBucket")
    asserts.equals(env, 4, len(buckets))
    for i, bucket in enumerate(buckets):
        names = _names(bucket)
        lines = [p for p in names if p.endswith(".log")]
        asserts.equals(env, 1, len(lines), "one bucket's lines")
        asserts.true(env, lines[0].endswith("bucket-00{}.log".format(i)), lines[0])
        for forbidden in ("corpus.log", "/base/", "MANIFEST.json", "/raw/"):
            asserts.equals(env, [], [p for p in names if forbidden in p], "a bucket's inputs naming " + forbidden)
        asserts.true(env, [p for p in names if p.endswith("libvulkan_nouveau.dylib.source")] != [], "the identity")
        unused = [f.short_path for f in bucket.outputs.to_list() if f.extension == "unused"]
        asserts.equals(env, 1, len(unused), "the unused inputs list")
        argv = bucket.argv
        asserts.equals(env, argv[argv.index("--nvk") + 1], argv[argv.index("--unused") + 1], "the twin is unused")
    (join,) = _actions(env, "MetalSeedJoin")
    asserts.equals(env, [], [p for p in _names(join) if p.endswith(".pack")], "the join reads no pack")
    asserts.equals(env, [], [p for p in _names(join) if "/base/" in p or "/tools/" in p], "the join reads no tools")
    (check,) = _actions(env, "MetalSeedCheck")
    asserts.true(env, [p for p in _names(check) if "/base/" in p] != [], "the bundle is checked")
    validation = analysistest.target_under_test(env)[OutputGroupInfo]._validation.to_list()
    (native_check,) = _actions(env, "MetalNativeCoverage")
    asserts.equals(
        env,
        check.outputs.to_list() + native_check.outputs.to_list(),
        validation,
        "compiler identity and native coverage are validation actions",
    )
    native_buckets = _actions(env, "MetalNativeBucket")
    asserts.equals(env, 4, len(native_buckets))
    for bucket in native_buckets:
        names = _names(bucket)
        asserts.equals(
            env,
            [],
            [p for p in names if p.endswith("MANIFEST.json")],
            "native compilation does not read the runtime stack",
        )
        asserts.true(env, "--store" in bucket.argv, "native compilation sees generic libraries")
        asserts.equals(env, 1, len([p for p in names if p.endswith(".log")]), "one native bucket input")
    (native_stamp,) = _actions(env, "MetalNativeStamp")
    asserts.true(
        env,
        [p for p in _names(native_stamp) if p.endswith("MANIFEST.json")] != [],
        "native stamping reads the current runtime identity",
    )
    return analysistest.end(env)

_seed_test = analysistest.make(_seed_impl)

def _seed_release_impl(ctx):
    env = analysistest.begin(ctx)
    (check,) = _actions(env, "MetalNativeCoverage")
    asserts.true(env, "--strict" in check.argv, "release requires complete observed native render coverage")
    return analysistest.end(env)

_seed_release_test = analysistest.make(_seed_release_impl, config_settings = {
    str(Label("//bazel:profile")): "release",
})

def _raw_impl(ctx):
    env = analysistest.begin(ctx)
    probes = _actions(env, "MetalRawProbe")
    asserts.equals(env, 16, len(probes))
    for i, probe in enumerate(probes):
        names = _ours(_names(probe))
        lines = [p for p in names if p.endswith(".log")]
        asserts.equals(env, ["bucket{}.log".format(i)], [p.rpartition("/")[2] for p in lines], "one bucket's states")
        asserts.equals(env, [], [p for p in names if p.endswith("MANIFEST.json")], "a probe reads no stack key")
        asserts.true(env, [p for p in names if p.endswith("libvulkan_nouveau.dylib")] != [], "the twin")
        for library in (
            "crawl/tree/data/0.metallib",
            "device/1.metallib",
            "crawl/tree/root/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib",
        ):
            asserts.true(env, "gpu/seed/test/" + library in names, "native compiler input " + library)
        asserts.true(env, "--data" in probe.argv, "harvested libraries reach the native probe")
        asserts.true(env, probe.outputs.to_list()[0].basename == "bucket{}".format(i), "named as rawpack.py knows it")
    for mnemonic in ("MetalRawSplit", "MetalRawLink"):
        (action,) = _actions(env, mnemonic)
        asserts.equals(env, [], [p for p in _names(action) if p.endswith("MANIFEST.json")], mnemonic + " reads no key")
    (link,) = _actions(env, "MetalRawLink")
    asserts.true(env, "gpu/seed/test/raw/splits.py" in _names(link), "the split layout generator")
    asserts.equals(env, 16, len([f for f in link.inputs.to_list() if f.is_directory]), "every bucket's probe runs")
    (stamp,) = _actions(env, "MetalRawStamp")
    asserts.true(env, [p for p in _names(stamp) if p.endswith("MANIFEST.json")] != [], "the stamp reads the key")
    return analysistest.end(env)

_raw_test = analysistest.make(_raw_impl)

def seed_inputs_tests(name, crawl, raw_pack, seed):
    """The tests, as one test_suite.

    Args:
        name: the test_suite's name.
        crawl: a shader_crawl over a stand-in tree.
        raw_pack: a seed_raw_pack over stand-in tools and generator.
        seed: a metal_seed of four buckets over stand-in inputs.
    """
    _crawl_test(name = name + "_crawl", target_under_test = crawl)
    _seed_test(name = name + "_seed", target_under_test = seed)
    _seed_release_test(name = name + "_release", target_under_test = seed)
    _raw_test(name = name + "_raw", target_under_test = raw_pack)
    native.test_suite(name = name, tests = [name + "_crawl", name + "_seed", name + "_raw", name + "_release"])
