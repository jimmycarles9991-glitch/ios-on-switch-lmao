"""Where the prelinker's steps read and write: PRELINK_WORK holds every intermediate (prelink-lists.json, ours.nm,
cpp26.json, sym/, symbolsets26.txt, closure.json, kernel-names.json, the discriminator cache). The other variables name
the inputs: the kernelcache, our kernel, iOS 17's kernelcache (the GOT names kexts imported there) and the arm64e
CLI."""
import json
import os

WORK = os.environ["PRELINK_WORK"]
KERNELCACHE = os.environ.get("PRELINK_KERNELCACHE", "")
OURS = os.environ.get("PRELINK_OURS", "")
KERNELCACHE_17 = os.environ.get("PRELINK_KERNELCACHE_17", "")
ARM64E = os.environ.get("PRELINK_ARM64E", "")
SLIDE = int(os.environ["PRELINK_SLIDE"], 0) if os.environ.get("PRELINK_SLIDE") else None


def work(name):
    return os.path.join(WORK, name)


def prelink_lists():
    """prelink.py's --root, --cut and --defer: the roots, {(component, dependency): pointer sites} and
    {component: [personality]}."""
    lists = json.load(open(work("prelink-lists.json")))
    return lists["roots"], {(c, d): sites for c, d, sites in lists["cuts"]}, lists["deferred"]
