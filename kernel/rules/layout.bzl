"""Where the kernel's sources and generated files live, and how their paths are spelled in what the build ships.

The layout move (S3) took the kernel tree's drivers/, kits/, diagnostics/ and overlay/ from out/native-a57/root-v1/tree/
to the repository root (same relative layout) and its generated files from that package's build/ to kernel/build/.
Paths reach the artifacts (__FILE__ strings, debug info, the kernel's stabs): COMPAT_* keep the spellings the kernel
had before the move, so the move changes no shipped byte but the linker's object paths. Dropping the compatibility
spellings is a later, separate change (old_spelling and the compat maps become identities).

tools/kernel/common.py mirrors this table for the tools (keep the two in step).
"""

PATH_ROOT = "/switch-ios"

# The kernel tree's location before the move: the old spelling of every moved path.
OLD_TREE = "out/native-a57/root-v1/tree"

# Where the kernel tree's sources are now ("" = the repository root), and the package of its generated files.
TREE_ROOT = ""
KERNEL_PACKAGE = "kernel"

# The tree's top-level directories that live at TREE_ROOT.
MOVED_ROOTS = ["drivers", "kits", "diagnostics", "overlay"]

# The kernel tree's own subpackages before the move: their outputs were below OLD_TREE/<package>/ already.
OLD_SUBPACKAGES = ["drivers/TegraGM20BMetal"]

def tree_path(path):
    """A path below the kernel tree, as it is now (TREE_ROOT/path)."""
    return (TREE_ROOT + "/" + path) if TREE_ROOT else path

def old_spelling(path, bin_dir = None):
    """The pre-move spelling of an execroot-relative path (unchanged if it was not moved)."""
    for root in MOVED_ROOTS:
        if path == root or path.startswith(root + "/"):
            return OLD_TREE + "/" + path
    if bin_dir and KERNEL_PACKAGE != OLD_TREE:
        new = bin_dir + "/" + KERNEL_PACKAGE + "/"
        if path.startswith(new):
            return bin_dir + "/" + OLD_TREE + "/" + path[len(new):]
    return path

def old_output_spelling(path):
    """The pre-move spelling of an output-directory-relative path (unchanged while nothing has moved).

    A moved package's outputs (the kernel package's, drivers/<X>'s, kits/<X>'s, diagnostics/switch's, overlay's) were
    the kernel tree package's: what the kernel link (kernel/xnu-<version>/config:xnu_kernel) names them by, so its
    debug map keeps its paths.

    Args:
      path: a path below an output directory (a File's short_path).

    Returns:
      Its spelling before the layout move.
    """
    if KERNEL_PACKAGE == OLD_TREE:
        return path
    parts = path.split("/")
    if parts[0] == KERNEL_PACKAGE:
        return "/".join([OLD_TREE] + parts[1:])
    if len(parts) > 2 and parts[0] in ("drivers", "kits"):
        if "/".join(parts[:2]) in OLD_SUBPACKAGES:
            return OLD_TREE + "/" + path
        return "/".join([OLD_TREE] + parts[2:])
    if parts[:2] == ["diagnostics", "switch"]:
        return "/".join([OLD_TREE] + parts[2:])
    if parts[0] == "overlay":
        return "/".join([OLD_TREE] + parts[1:])
    return path

def compat_prefix_maps(bin_dir):
    """Prefix maps from moved paths to their old spellings.

    Macro maps (the most specific prefix wins) and debug maps (the last match wins, so these go after the generic
    ones). Inert while nothing lives at the new paths.
    """
    macro, debug = [], []
    if TREE_ROOT != OLD_TREE:
        for root in MOVED_ROOTS:
            macro.append("-fmacro-prefix-map={r}/={p}/{o}/{r}/".format(r = root, p = PATH_ROOT, o = OLD_TREE))
            debug.append("-fdebug-prefix-map={r}/={p}/{o}/{r}/".format(r = root, p = PATH_ROOT, o = OLD_TREE))
    if KERNEL_PACKAGE != OLD_TREE:
        new = "{}/{}/build".format(bin_dir, KERNEL_PACKAGE)
        macro.append("-fmacro-prefix-map={}={}/{}/build".format(new, PATH_ROOT, OLD_TREE))
        debug.append("-fdebug-prefix-map={}={}/{}/{}/build".format(new, PATH_ROOT, bin_dir, OLD_TREE))
    return macro, debug
