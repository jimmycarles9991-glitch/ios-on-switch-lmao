"""Analysis tests of SWITCH_DEBUG: user-space compiles get 1 in the debug profile and 0 in release or with
--//bazel:diagnostics=false, exactly once."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("//gpu:defs.bzl", "metal_plugin")
load("//kernel/rules:package.bzl", "ios_binary")

def _expect(env, mnemonic, wanted):
    actions = [a for a in analysistest.target_actions(env) if a.mnemonic == mnemonic]
    asserts.true(env, len(actions) > 0, "no {} action".format(mnemonic))
    for action in actions:
        found = [a for a in action.argv if a.startswith("-DSWITCH_DEBUG")]
        asserts.equals(env, [wanted], found, "{}'s SWITCH_DEBUG".format(mnemonic))

def _make(mnemonic, profile, wanted, diagnostics = True):
    def impl(ctx):
        env = analysistest.begin(ctx)
        _expect(env, mnemonic, wanted)
        return analysistest.end(env)

    return analysistest.make(impl, config_settings = {
        str(Label("//bazel:profile")): profile,
        str(Label("//bazel:diagnostics")): diagnostics,
    })

_ios_debug_test = _make("IosBinary", "debug", "-DSWITCH_DEBUG=1")
_ios_release_test = _make("IosBinary", "release", "-DSWITCH_DEBUG=0")
_ios_no_diagnostics_test = _make("IosBinary", "debug", "-DSWITCH_DEBUG=0", diagnostics = False)
_plugin_debug_test = _make("ObjcxxCompile", "debug", "-DSWITCH_DEBUG=1")
_plugin_release_test = _make("ObjcxxCompile", "release", "-DSWITCH_DEBUG=0")
_plugin_no_diagnostics_test = _make("ObjcxxCompile", "debug", "-DSWITCH_DEBUG=0", diagnostics = False)

def profile_tests(name, source, objcxx_source):
    """The tests, as one test_suite.

    Args:
        name: the test_suite's name.
        source: any C source (only analysed, never compiled).
        objcxx_source: any .mm source (only analysed, never compiled).
    """
    ios_binary(
        name = name + "_ios_binary",
        srcs = [source],
        out = name + "/ios_binary",
        tags = ["manual"],
    )
    metal_plugin(
        name = name + "_plugin",
        srcs = [objcxx_source],
        compile = source,
        tags = ["manual"],
        translate = source,
    )
    tests = {
        "_ios_binary_debug": (_ios_debug_test, "_ios_binary"),
        "_ios_binary_release": (_ios_release_test, "_ios_binary"),
        "_ios_binary_no_diagnostics": (_ios_no_diagnostics_test, "_ios_binary"),
        "_plugin_debug": (_plugin_debug_test, "_plugin"),
        "_plugin_release": (_plugin_release_test, "_plugin"),
        "_plugin_no_diagnostics": (_plugin_no_diagnostics_test, "_plugin"),
    }
    for suffix, (test, target) in tests.items():
        test(name = name + suffix, target_under_test = ":" + name + target)
    native.test_suite(
        name = name,
        tests = [name + suffix for suffix in tests],
    )
