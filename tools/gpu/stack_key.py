#!/usr/bin/env python3
"""Write the driver's stack key.

usage: stack_key.py @<params>   with --ipsw <build> --out <file> and --file <path below the tree>=<path>, one per file

The stack key is known before the plugin is compiled: it is compiled in (-DTEGRA_STACK_KEY), stamped into the pack
and the native seed, and names each process's record cache, so a new key retires every record made before. It hashes the
IPSW build and each file gpu/defs.bzl stack_key names (what records are made and read by), each by its path below the
tree, its length and its canonical text, in path order. Writes the first 16 hex digits.

A source's canonical text is what its compiler reads. C, C++, Objective-C and Metal: their tokens without comments, one
space wherever layout parted two (the preprocessor reads layout between tokens: `#define A (x)` is not `#define A(x)`,
a stringified argument keeps its spaces, a header name is its characters) and each directive's line end. Rust: its
tokens and doc comments, a space only where layout parted two words or two joining marks (the token stream records no
other spacing).
Python: its syntax tree. So a comment, a re-indent or a rewrapped line keeps every record the device made, and any
other edit retires them. Every other file is hashed by its bytes.
"""

import argparse
import ast
import hashlib
import re
from pathlib import Path, PurePosixPath

SCHEME = b'raw stack 3'
C_FAMILY = {'.c', '.h', '.cpp', '.mm', '.m', '.metal'}

# One token or gap at a time. A string or character literal that reaches the end of its line unclosed ends there, as
# the compiler would refuse it: a stray quote (an apostrophe in an #error) never runs on into the next line's code.
C_TOKEN = re.compile(
    r"""(?P<nl>\n)|(?P<ws>[ \t\f\v\r]+)|(?P<comment>//[^\n]*|/\*.*?\*/)
    |(?P<literal>(?:u8|[uUL])?R"(?P<delim>[^ ()\\\t\v\f\n]{0,16})\(.*?\)(?P=delim)"
        |(?:u8|[uUL@])?"(?:[^"\\\n]|\\.)*"?|(?:u8|[uUL])?'(?:[^'\\\n]|\\.)*'?
        |\.?[0-9](?:[eEpP][+-]|['\w.])*)
    |(?P<word>\w+)|(?P<punct>.)""",
    re.VERBOSE | re.DOTALL,
)
RUST_TOKEN = re.compile(
    r"""(?P<ws>\s+)|(?P<comment>//[^\n]*)|(?P<open>/\*)
    |(?P<literal>[bc]?r(?P<hashes>\#*)".*?"(?P=hashes)|[bc]?"(?:[^"\\]|\\.)*"
        |b?'(?:\\(?:u\{[0-9a-fA-F_]*\}|x[0-9a-fA-F]{2}|.)|[^'\\\n])')
    |(?P<word>'?\w+)|(?P<punct>.)""",
    re.VERBOSE | re.DOTALL,
)
BLOCK_EDGE = re.compile(r'/\*|\*/')
# A Rust doc comment is an attribute (#[doc]) a macro may read, so it stays: ///, //!, /** and /*!, but not ////, /**/
# or /***
RUST_DOC = re.compile(r'//(?:/(?!/)|!)|/\*(?:\*(?![*/])|!)')
# Punctuation that joins its neighbour into another token (a - -b and a--b; > > and >>), Rust's Joint spacing; a
# bracket, comma or semicolon joins none
JOINERS = frozenset('+-*/%&|^!=<>.:#?~@$\\')


def rust_block_end(text: str, at: int) -> int:
    """Give where the block comment opened just before `at` ends: Rust's nest."""
    depth = 1
    for m in BLOCK_EDGE.finditer(text, at):
        depth += 1 if m[0] == '/*' else -1
        if not depth:
            return m.end()
    return len(text)


def token(text: str, rust: bool, at: int) -> tuple[str, str, int]:
    """Give the token or gap at `at`: its kind (nl, gap, w a word, j a joiner, b other punctuation), text and end."""
    m = (RUST_TOKEN if rust else C_TOKEN).match(text, at)
    kind, end = m.lastgroup, m.end()
    if kind == 'open':
        end = rust_block_end(text, end)
    if kind in ('open', 'comment'):
        return ('w', text[at:end].rstrip(), end) if rust and RUST_DOC.match(text, at) else ('gap', '', end)
    if kind in ('nl', 'ws'):
        return 'nl' if kind == 'nl' else 'gap', '', end
    return 'w' if kind != 'punct' else 'j' if m[0] in JOINERS else 'b', m[0], end


def tokens(text: str, rust: bool) -> str:
    """Give `text`'s tokens: C's with one space wherever layout parted two, Rust's only where two would join.

    In Rust two words (names, numbers, literals) join, and two JOINERS do; no other layout reaches a token. A C
    preprocessor directive ends at its line's end, which is kept; a spliced line is the line it continues.
    """
    if not rust:
        text = text.replace('\\\n', '')
    out: list[str] = []
    gap, previous, directive, line_start = False, '', False, True
    at = 0
    while at < len(text):
        kind, word, at = token(text, rust, at)
        if kind == 'nl' and directive:
            out.append('\n')
            gap, previous, directive, line_start = False, '', False, True
            continue
        if kind in ('nl', 'gap'):
            gap, line_start = True, line_start or kind == 'nl'
            continue
        if not rust and line_start and word == '#':
            directive = True
            if out and out[-1] != '\n':
                out.append('\n')
            gap, previous = False, ''
        if gap and previous and (not rust or kind == previous != 'b'):
            out.append(' ')
        out.append(word)
        gap, previous, line_start = False, kind, False
    return ''.join(out)


def canonical(name: str, data: bytes) -> bytes:
    """Give the text of the file below the tree at `name` that the key hashes: see the module's docstring."""
    suffix = PurePosixPath(name).suffix
    if suffix not in C_FAMILY | {'.rs', '.py'}:
        return data
    try:
        text = data.decode()
        return (ast.dump(ast.parse(text)) if suffix == '.py' else tokens(text, suffix == '.rs')).encode()
    except (UnicodeDecodeError, SyntaxError, ValueError):
        return data


def key(ipsw: str, files: dict[str, Path]) -> str:
    """Give the stack key of `files` (each file's path below the tree: where it is) under IPSW build `ipsw`."""
    digest = hashlib.sha256(SCHEME + b'\0' + ipsw.encode() + b'\0')
    for name in sorted(files):
        data = canonical(name, files[name].read_bytes())
        digest.update(name.encode() + b'\0' + len(data).to_bytes(8, 'little') + data)
    return digest.hexdigest()[:16]


def main() -> None:
    parser = argparse.ArgumentParser(fromfile_prefix_chars='@')
    parser.add_argument('--ipsw', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--file', action='append', default=[], help='<path below the tree>=<path>')
    args = parser.parse_args()
    files = dict(entry.split('=', 1) for entry in args.file)
    if len(files) != len(args.file):
        raise SystemExit('stack_key.py: a path below the tree is named twice')
    Path(args.out).write_text(key(args.ipsw, {name: Path(path) for name, path in files.items()}) + '\n')


if __name__ == '__main__':
    main()
