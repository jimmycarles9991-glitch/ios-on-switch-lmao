"""The hardware test apps' rules.

    hwtest_app(name, minimum_os_version, ...)   one iOS version's app: ad hoc signed, iPad, the shaders as its library
    ios_arm64e(name, app)                        an ios_application's .ipa built for arm64e alone, whatever
                                                 --ios_multi_cpus says (.bazelrc: arm64)

rules_apple takes an app's architectures from --ios_multi_cpus only, so a per-app arm64e build needs ios_arm64e's
outgoing transition.
"""

load("@rules_apple//apple:ios.bzl", "ios_application")

def hwtest_app(name, minimum_os_version, deps, bundle_id, bundle_name, entitlements, infoplist, **kwargs):
    """One iOS version's hardware test app.

    Ad hoc signed like the overlay's other code (--@rules_apple//apple/build_settings:signing_certificate_name=- in
    .bazelrc), so its CDHashes go in the boot package's trust cache. rules_apple wants a profile for any device build:
    adhoc.mobileprovision is a plain-XML stand-in. It lands in the bundle as embedded.mobileprovision, which nothing
    reads for a trust-cached platform app.

    Args:
        name: the target.
        minimum_os_version: the iOS version the app is built for.
        deps: its swift_library, with the @main.
        bundle_id: the bundle identifier.
        bundle_name: the bundle's and the executable's name.
        entitlements: the entitlements plist.
        infoplist: the Info.plist.
        **kwargs: passed to ios_application (tags, visibility).
    """
    ios_application(
        name = name,
        bundle_id = bundle_id,
        bundle_name = bundle_name,
        entitlements = entitlements,
        entitlements_validation = "skip",
        families = ["ipad"],
        infoplists = [infoplist],
        minimum_os_version = minimum_os_version,
        provisioning_profile = "adhoc.mobileprovision",
        resources = native.glob(["shaders/*.metal"]),
        deps = deps,
        **kwargs
    )

def _arm64e_transition_impl(_settings, _attr):
    return {"//command_line_option:ios_multi_cpus": ["arm64e"]}

_arm64e_transition = transition(
    implementation = _arm64e_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:ios_multi_cpus"],
)

def _ios_arm64e_impl(ctx):
    return [DefaultInfo(files = ctx.attr.app[0][DefaultInfo].files)]

ios_arm64e = rule(
    implementation = _ios_arm64e_impl,
    attrs = {
        "app": attr.label(
            cfg = _arm64e_transition,
            mandatory = True,
            doc = "the ios_application to build for arm64e",
        ),
    },
    doc = "An ios_application's .ipa, built for arm64e alone.",
)
