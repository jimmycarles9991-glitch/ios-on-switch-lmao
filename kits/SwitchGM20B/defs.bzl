"""What a kernel driver adds to use kits/SwitchGM20B (the kernel toolchain passes no cc_library include paths, and
NVIDIA's class headers include "nvtypes.h" from Mesa's headers directory, not their own), and the capability table
generator's inputs."""

SWITCH_GM20B_INCLUDES = ["-I", "third_party/mesa/src/nouveau/headers"]

_CLASSES = "//third_party/mesa/src/nouveau/headers:nvidia/classes/"

# The NVIDIA headers the kit's umbrella headers take, from Mesa's vendored copy.
SWITCH_GM20B_MESA_HEADERS = [
    "//third_party/mesa/src/nouveau/headers:drf.h",
    "//third_party/mesa/src/nouveau/headers:nvtypes.h",
] + [_CLASSES + c + ".h" for c in [
    "cl902d",
    "cl906f",
    "cla140",
    "clb06f",
    "clb0b5",
    "clb197",
    "clb197tex",
    "clb1c0",
    "clb1c0qmd",
]]

# What gen_caps.py reads: the tables, the class headers it resolves names in (with the Fermi and Maxwell A spellings
# NIL's generator emits), NIL's format table and generator, Mesa's format descriptions, and the HW Test sources whose
# test ids rows may name.
SWITCH_GM20B_CAPS_INPUTS = [
    "//kits/SwitchGM20B:caps/features.tsv",
    "//kits/SwitchGM20B:caps/formats.tsv",
    "//kits/SwitchGM20B:caps/limits.tsv",
    "//kits/SwitchGM20B:caps/methods.tsv",
    "//kits/SwitchGM20B:SwitchGM20BTegra.h",
    "//third_party/mesa/src/nouveau/nil:nil_formats.csv",
    "//third_party/mesa/src/nouveau/nil:nil_format_table_gen.py",
    "//third_party/mesa/src/util/format:u_format.yaml",
    "//hw_test_app:test_sources",
] + SWITCH_GM20B_MESA_HEADERS + [_CLASSES + c + ".h" for c in ["cl9097", "cl9097tex", "clb097", "clb097tex"]]

def caps_args(location = "execpath"):
    """gen_caps.py's arguments over SWITCH_GM20B_CAPS_INPUTS, as $(execpath) (genrules) or $(rootpath) (runfiles).

    Args:
        location: the make-variable function that names each input.

    Returns:
        The arguments, for a shell command.
    """

    def loc(label):
        return "$({} {})".format(location, label)

    return " ".join([
        "--tables $$(dirname {})".format(loc("//kits/SwitchGM20B:caps/formats.tsv")),
        "--classes $$(dirname {})".format(loc(_CLASSES + "clb197.h")),
        "--tegra " + loc("//kits/SwitchGM20B:SwitchGM20BTegra.h"),
        "--nil-csv " + loc("//third_party/mesa/src/nouveau/nil:nil_formats.csv"),
        "--nil-gen " + loc("//third_party/mesa/src/nouveau/nil:nil_format_table_gen.py"),
        "--u-format " + loc("//third_party/mesa/src/util/format:u_format.yaml"),
        "$$(for f in $({}s //hw_test_app:test_sources); do echo --hwtest-source $$f; done)".format(location),
    ])
