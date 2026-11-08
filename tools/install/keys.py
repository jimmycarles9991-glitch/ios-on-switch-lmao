"""What each stage of //:image is made from, as a key recorded beside the stage's output.

    compose   the IPSW's OS image, the spec's volumes, the nominal container size
    populate  compose's key, the cryptexes, the boot package's overlay, the GPU trust cache, the shader seed, the
              pretranslate drivers and their pins
    pack      populate's key
    head      pack's key, the version's boot files and label

A key holds its stage's VERSIONS entry: changing what a stage's code makes means bumping it. A rerun skips every stage
whose key is unchanged and reruns from the first that changed; when only the head changed, only NAME.SII.001's head
is rewritten.
"""

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

from tools.common import hashing

VERSIONS = {'compose': 1, 'populate': 2, 'pack': 1, 'head': 1}
STAGES = ('compose', 'populate', 'pack', 'head')


def digest(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def tree_digest(root: str | Path | None) -> str | None:
    """Digest every file's path, mode and bytes under `root` (a file is its own digest) on every core."""
    if not root or not Path(root).exists():
        return None
    root = Path(root)
    if root.is_file():
        return hashing.sha256_file(root)
    paths = sorted(p for p in root.rglob('*') if p.is_file() and not p.name.startswith('._'))
    hashes = hashing.sha256_files(paths)
    entries = [
        (p.relative_to(root).as_posix(), p.stat().st_mode & 0o111, h) for p, h in zip(paths, hashes, strict=True)
    ]
    return digest(entries)


@dataclass(frozen=True)
class Keys:
    compose: str
    populate: str
    pack: str
    head: str


def compute(inputs: dict, boot_files: list[tuple[str, Path]], label: str) -> Keys:
    """Compute the stages' keys from their inputs as plain values (stages.key_inputs) and the boot files."""
    compose = digest({'version': VERSIONS['compose'], **inputs['compose']})
    populate = digest({'version': VERSIONS['populate'], 'compose': compose, **inputs['populate']})
    pack = digest({'version': VERSIONS['pack'], 'populate': populate})
    boot = [(name, hashing.sha256_file(path)) for name, path in boot_files]
    head = digest({'version': VERSIONS['head'], 'pack': pack, 'boot': boot, 'label': label})
    return Keys(compose, populate, pack, head)


def first_stage(keys: Keys, scratch: dict, record: dict | None) -> str | None:
    """Return the first stage a run must do, None when the image is up to date.

    `scratch` maps the stages whose output is still in the scratch directory to their keys; `record` is what the
    folder holds of the image (its pack and head keys), None when it holds none.
    """
    if record and record.get('pack') == keys.pack:
        return None if record.get('head') == keys.head else 'head'
    if scratch.get('populate') == keys.populate:
        return 'pack'
    if scratch.get('compose') == keys.compose:
        return 'populate'
    return 'compose'
