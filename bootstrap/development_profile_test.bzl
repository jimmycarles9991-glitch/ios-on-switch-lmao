"""Analysis tests: debug payloads include hermetic development SSH without any keys; release payloads include none."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

def _payload_action(env):
    actions = [a for a in analysistest.target_actions(env) if a.mnemonic == "BootstrapPayload"]
    asserts.equals(env, 1, len(actions), "one payload action")
    return actions[0]

def _takes_development(action):
    return "--development" in action.argv or any([f.basename == "development.packages" for f in action.inputs.to_list()])

def _has_key_inputs(action):
    return any([
        "authorized_keys" in f.path or "ssh_keys" in f.path or "ssh_host" in f.path
        for f in action.inputs.to_list()
    ])

def _release_impl(ctx):
    env = analysistest.begin(ctx)
    action = _payload_action(env)
    asserts.false(env, _takes_development(action), "a release payload takes the development SSH set")
    asserts.false(env, _has_key_inputs(action), "a release payload takes SSH key inputs")
    return analysistest.end(env)

def _debug_impl(ctx):
    env = analysistest.begin(ctx)
    action = _payload_action(env)
    asserts.true(env, _takes_development(action), "a debug payload lacks the development SSH set")
    asserts.false(env, _has_key_inputs(action), "a debug payload takes SSH key inputs")
    return analysistest.end(env)

release_payload_test = analysistest.make(_release_impl, config_settings = {str(Label("//bazel:profile")): "release"})
debug_payload_test = analysistest.make(_debug_impl, config_settings = {str(Label("//bazel:profile")): "debug"})
