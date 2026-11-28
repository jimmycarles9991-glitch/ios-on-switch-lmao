"""Analysis tests for //bazel:hwtest gating on //boot:package and //overlay:jetsam_properties."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

def _action(env, mnemonic):
    actions = [a for a in analysistest.target_actions(env) if a.mnemonic == mnemonic]
    asserts.equals(env, 1, len(actions), "expected one {} action".format(mnemonic))
    return actions[0]

def _given_sources(plan_action):
    argv = plan_action.argv
    return [argv[i + 1] for i in range(len(argv) - 1) if argv[i] == "--given"]

def _has_hwtest_input(action):
    return any([
        "SwitchHWTest" in f.path or "switch-texpeer" in f.path
        for f in action.inputs.to_list()
    ])

def _make_package_test(kind, profile, hwtest, expect_hwtest, expect_debug):
    def impl(ctx):
        env = analysistest.begin(ctx)
        plan = _action(env, "OverlayPlan")
        trust = _action(env, "OverlayTrust")
        tree = _action(env, "OverlayTree")
        given = _given_sources(plan)

        asserts.equals(env, not expect_debug, "--release" in plan.argv, "OverlayPlan --release")
        asserts.equals(env, expect_hwtest, "--hwtest" in plan.argv, "OverlayPlan --hwtest")
        asserts.equals(env, not expect_hwtest, "--no-hwtest" in plan.argv, "OverlayPlan --no-hwtest")

        asserts.equals(env, expect_hwtest, "build/switch-hwtest/SwitchHWTest.ipa" in given, "SwitchHWTest.ipa in --given")
        asserts.equals(env, expect_hwtest, "build/switch-hwtest/switch-texpeer" in given, "switch-texpeer in --given")
        asserts.equals(
            env,
            expect_hwtest and kind == "24B119",
            "build/switch-hwtest/SwitchHWTest27e.ipa" in given,
            "SwitchHWTest27e.ipa in --given",
        )
        asserts.equals(env, expect_debug, "build/switch-gpu/fetchform" in given, "fetchform in --given")
        asserts.equals(
            env,
            expect_debug,
            "build/switch-bootstrap/packages/switch-ssh.deb" in given,
            "switch-ssh.deb in --given",
        )

        asserts.equals(env, expect_hwtest, _has_hwtest_input(trust), "OverlayTrust hwtest inputs")
        asserts.equals(env, expect_hwtest, _has_hwtest_input(tree), "OverlayTree hwtest inputs")
        return analysistest.end(env)

    return analysistest.make(impl, config_settings = {
        str(Label("//ipsw:kind")): kind,
        str(Label("//bazel:profile")): profile,
        str(Label("//bazel:hwtest")): hwtest,
    })

def _make_jetsam_test(profile, hwtest, expect_texpeer, prebuilt = "local"):
    def impl(ctx):
        env = analysistest.begin(ctx)
        actions = analysistest.target_actions(env)
        asserts.equals(env, 1, len(actions), "expected one jetsam_properties action")
        has_peer = any([
            f.basename == "com.brunostjohn.switch.texpeer.jetsam.plist"
            for f in actions[0].inputs.to_list()
        ])
        asserts.equals(env, expect_texpeer, has_peer, "texpeer jetsam fragment")
        return analysistest.end(env)

    return analysistest.make(impl, config_settings = {
        str(Label("//bazel:profile")): profile,
        str(Label("//bazel:hwtest")): hwtest,
        str(Label("//install:prebuilt")): prebuilt,
    })

_ios17_debug_default_test = _make_package_test("21H461", "debug", "auto", expect_hwtest = True, expect_debug = True)
_ios17_debug_disabled_test = _make_package_test("21H461", "debug", "false", expect_hwtest = False, expect_debug = True)
_ios17_release_default_test = _make_package_test("21H461", "release", "auto", expect_hwtest = False, expect_debug = False)
_ios17_release_enabled_test = _make_package_test("21H461", "release", "true", expect_hwtest = True, expect_debug = False)
_ios27_debug_default_test = _make_package_test("24B119", "debug", "auto", expect_hwtest = True, expect_debug = True)
_ios27_debug_disabled_test = _make_package_test("24B119", "debug", "false", expect_hwtest = False, expect_debug = True)
_ios27_release_default_test = _make_package_test("24B119", "release", "auto", expect_hwtest = False, expect_debug = False)
_ios27_release_enabled_test = _make_package_test("24B119", "release", "true", expect_hwtest = True, expect_debug = False)

_jetsam_debug_default_test = _make_jetsam_test("debug", "auto", expect_texpeer = True)
_jetsam_debug_disabled_test = _make_jetsam_test("debug", "false", expect_texpeer = False)
_jetsam_release_default_test = _make_jetsam_test("release", "auto", expect_texpeer = False)
_jetsam_release_enabled_test = _make_jetsam_test("release", "true", expect_texpeer = True)
_jetsam_prebuilt_release_test = _make_jetsam_test("debug", "auto", expect_texpeer = False, prebuilt = "release")

def hwtest_package_tests(name):
    """Registers analysis tests verifying //bazel:hwtest gating on //boot:package and //overlay:jetsam_properties.

    Args:
        name: name of the generated test_suite.
    """
    package_tests = {
        "ios17_debug_default": _ios17_debug_default_test,
        "ios17_debug_disabled": _ios17_debug_disabled_test,
        "ios17_release_default": _ios17_release_default_test,
        "ios17_release_enabled": _ios17_release_enabled_test,
        "ios27_debug_default": _ios27_debug_default_test,
        "ios27_debug_disabled": _ios27_debug_disabled_test,
        "ios27_release_default": _ios27_release_default_test,
        "ios27_release_enabled": _ios27_release_enabled_test,
    }
    jetsam_tests = {
        "jetsam_debug_default": _jetsam_debug_default_test,
        "jetsam_debug_disabled": _jetsam_debug_disabled_test,
        "jetsam_release_default": _jetsam_release_default_test,
        "jetsam_release_enabled": _jetsam_release_enabled_test,
        "jetsam_prebuilt_release": _jetsam_prebuilt_release_test,
    }
    tests = []
    for suffix, test_rule in package_tests.items():
        test_name = name + "_" + suffix
        test_rule(name = test_name, target_under_test = ":package")
        tests.append(test_name)
    for suffix, test_rule in jetsam_tests.items():
        test_name = name + "_" + suffix
        test_rule(name = test_name, target_under_test = "//overlay:jetsam_properties")
        tests.append(test_name)
    native.test_suite(
        name = name,
        tests = tests,
    )
