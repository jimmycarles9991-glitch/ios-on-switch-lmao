"""Analysis tests of what the seed's actions are keyed on, over stand-in inputs (no IPSW is fetched):

- the crawl's names read the crawled tree and the tool alone;
- the replies' layout reads neither the tools nor the GPU bundle;
- the bundle check is a validation action, so the GPU bundle is no input of anything that makes the seed;
- the pack's probes read their bucket's states, directbucket.py, directpack and the mock (bucket 0 also own.metallib
  and context.log), the libraries and their own tool, and nothing else: not the stack key, the generator's other
  modules, make_pack.py's other steps or the tools they do not run, so an edit to any of those reruns no probe; only the
  stamp reads the tools' manifest (the stack key);
- a native bucket reads native-seed and the mock of the raw tools, none of the others;
- of a crawl with our own apps, only the last bucket (the apps' states) reads the tree with their libraries: the others
  read the tree without them, so an app's shader edit reruns one probe.
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
    (replies,) = _actions(env, "MetalSeedReplies")
    asserts.equals(env, [], [p for p in _names(replies) if "/base/" in p or "/tools/" in p], "the replies read no tools")
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
            ["gpu/seed/test/tools/raw/libmock_iokit.dylib", "gpu/seed/test/tools/raw/native-seed"],
            sorted([p for p in names if "/tools/raw/" in p]),
            "native compilation reads native-seed and the mock of the raw tools",
        )
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

_PROBE_TOOLS = ["directpack", "libmock_iokit.dylib"]

def _probe_read(path, i):
    """Whether bucket i's probe may read `path`: what can change the records it makes, and its own tool."""
    tools = _PROBE_TOOLS + (["own.metallib", "context.log"] if i == 0 else [])
    return (
        path.endswith("/split/bucket{}.log".format(i)) or
        path == "gpu/seed/test/pack/directbucket.py" or
        path in ["gpu/seed/test/tools/raw/" + t for t in tools] or
        path.startswith(("gpu/seed/test/crawl/tree/", "gpu/seed/test/device/")) or
        path.startswith("tools/gpu/") and "probe" in path
    )

def _pack_impl(ctx):
    env = analysistest.begin(ctx)
    probes = _actions(env, "MetalRawProbe")
    asserts.equals(env, 17, len(probes))
    for i, probe in enumerate(probes):
        names = _ours(_names(probe))
        lines = [p for p in names if "/split/" in p]
        asserts.equals(env, ["bucket{}.log".format(i)], [p.rpartition("/")[2] for p in lines], "one bucket's states")
        asserts.equals(env, [], [p for p in names if not _probe_read(p, i)], "bucket {} reads beyond its records' makers".format(i))
        asserts.equals(env, [], [p for p in names if p.endswith("MANIFEST.json")], "a probe reads no stack key")
        for library in (
            "crawl/tree/data/0.metallib",
            "device/1.metallib",
            "crawl/tree/root/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib",
        ):
            asserts.true(env, "gpu/seed/test/" + library in names, "native compiler input " + library)
        asserts.true(env, "--data" in probe.argv, "harvested libraries reach the native probe")
        asserts.true(env, probe.outputs.to_list()[0].basename == "bucket{}".format(i), "named as make_pack.py knows it")
    for mnemonic in ("MetalRawSplit", "MetalRawLink"):
        (action,) = _actions(env, mnemonic)
        asserts.equals(env, [], [p for p in _names(action) if p.endswith("MANIFEST.json")], mnemonic + " reads no key")
    (link,) = _actions(env, "MetalRawLink")
    asserts.true(env, "gpu/seed/test/pack/splits.py" in _names(link), "the split layout generator")
    asserts.equals(env, 17, len([f for f in link.inputs.to_list() if f.is_directory]), "every bucket's probe runs")
    (stamp,) = _actions(env, "MetalRawStamp")
    asserts.true(env, [p for p in _names(stamp) if p.endswith("MANIFEST.json")] != [], "the stamp reads the key")
    return analysistest.end(env)

_pack_test = analysistest.make(_pack_impl)

def _pack_apps_impl(ctx):
    env = analysistest.begin(ctx)
    probes = _actions(env, "MetalRawProbe")
    asserts.equals(env, 17, len(probes))
    for i, probe in enumerate(probes):
        trees = sorted([f.basename.rpartition("_")[2] for f in probe.inputs.to_list() if f.is_directory])
        asserts.equals(env, ["inputs"] if i == 16 else ["base"], trees, "bucket {}'s libraries".format(i))
    return analysistest.end(env)

_pack_apps_test = analysistest.make(_pack_apps_impl)

def seed_inputs_tests(name, crawl, pack, pack_apps, seed):
    """The tests, as one test_suite.

    Args:
        name: the test_suite's name.
        crawl: a shader_crawl over a stand-in tree.
        pack: a seed_pipeline_pack over stand-in tools and generator.
        pack_apps: the same over a crawl with an application.
        seed: a metal_seed of four buckets over stand-in inputs.
    """
    _crawl_test(name = name + "_crawl", target_under_test = crawl)
    _seed_test(name = name + "_seed", target_under_test = seed)
    _seed_release_test(name = name + "_release", target_under_test = seed)
    _pack_test(name = name + "_pack", target_under_test = pack)
    _pack_apps_test(name = name + "_pack_apps", target_under_test = pack_apps)
    native.test_suite(
        name = name,
        tests = [name + "_crawl", name + "_seed", name + "_pack", name + "_pack_apps", name + "_release"],
    )
