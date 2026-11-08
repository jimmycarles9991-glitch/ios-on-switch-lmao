"""The kernel's sysctl lists (struct sysctl_oid_list), named by the sysctl tree Apple's kernel builds.

A node (struct sysctl_oid: oid_parent at 0, oid_arg1 at 24, oid_name at 40) hangs off its parent list, and a node
of type CTLTYPE_NODE holds its own children's list in oid_arg1. Walking names up from a list gives its path, and XNU
names the list sysctl__<path>_children (the root's: sysctl__children). Kexts hang their nodes off these lists, so a
list named by the GOT order of some kext's imports alone could be vfs's where apfs's nodes belong under vfs.generic.
"""
import collections

OID_PARENT, OID_KIND, OID_ARG1, OID_NAME, OID_HANDLER, OID_FMT = 0, 20, 24, 40, 48, 56
CTLTYPE_NODE = 1
# sysctl.h's format strings and the generic handler of each
HANDLERS = {"I": "_sysctl_handle_int", "IU": "_sysctl_handle_int", "L": "_sysctl_handle_long",
            "LU": "_sysctl_handle_long", "Q": "_sysctl_handle_quad", "QU": "_sysctl_handle_quad",
            "A": "_sysctl_handle_string"}


def _cstring(kc, va):
    at = kc.va2off(va)
    if at is None:
        return None
    end = kc.d.find(b"\0", at, at + 64)
    text = kc.d[at:end] if end > at else b""
    if not text or not all(0x21 <= c < 0x7F for c in text):
        return None
    return text.decode()


def name(kc, fix, in_kernel, is_function):
    """{Apple kernel address: name}: lists ('_sysctl__<path>_children'); the kernel variables behind leaf nodes (a
    kext's or the kernel's), which XNU mostly names as the leaf (audio_active's int) unless more than one variable
    answers to the leaf name; the generic handlers (sysctl_handle_quad: every node using it declares "Q")"""
    nodes, variables, formats = {}, {}, {}
    for name_site in fix:
        site = name_site - OID_NAME
        label = _cstring(kc, fix[name_site][0])
        if label is None:
            continue
        kind = int.from_bytes(kc.d[kc.va2off(site + OID_KIND):kc.va2off(site + OID_KIND) + 4], "little")
        arg1 = fix[site + OID_ARG1][0] if site + OID_ARG1 in fix else None
        handler = fix[site + OID_HANDLER][0] if site + OID_HANDLER in fix else None
        fmt = _cstring(kc, fix[site + OID_FMT][0]) if site + OID_FMT in fix else None
        if handler is not None and is_function(handler):
            formats.setdefault(handler, collections.Counter())[fmt] += 1
        # a leaf's variable; the oid may get its parent only when registered (no oid_parent pointer here)
        if (kind & 0xF != CTLTYPE_NODE and handler is not None and is_function(handler) and arg1 is not None
                and in_kernel(arg1) and not is_function(arg1)):
            variables.setdefault(arg1, set()).add("_" + label)
        parent = fix[site][0] if site in fix else None
        if not in_kernel(site) or parent is None or not in_kernel(parent) or is_function(parent):
            continue
        nodes[site] = (parent, label, arg1 if kind & 0xF == CTLTYPE_NODE else None)
    owner = {}
    for node, (_, _, children) in nodes.items():
        if children is not None:
            owner.setdefault(children, set()).add(node)
    lists = {parent for parent, _, _ in nodes.values()}
    # a list owned by no node found here has no path, except the root: the one holding the top-level nodes
    top = {"kern", "vm", "vfs", "net", "debug", "hw", "machdep", "user", "security"}
    under = {lst: {label for parent, label, _ in nodes.values() if parent == lst} for lst in lists}
    roots = sorted((len(under[lst] & top), lst) for lst in lists if not owner.get(lst))
    root = roots[-1][1] if roots and roots[-1][0] >= 4 else None
    paths = {}

    def path(lst, depth=0):
        if lst in paths:
            return paths[lst]
        owners = owner.get(lst, set())
        if lst == root:
            result = ()
        elif depth > 16 or len(owners) != 1:
            result = None
        else:
            parent, label, _ = nodes[next(iter(owners))]
            up = path(parent, depth + 1)
            result = None if up is None else up + (label,)
        paths[lst] = result
        return result

    out = {}
    for lst in lists:
        p = path(lst)
        if p is not None:
            out[lst] = "_sysctl__" + "".join(part + "_" for part in p) + "children"
    claimed = collections.Counter(n for names in variables.values() for n in names)
    for v, n in variables.items():
        if len(n) == 1 and claimed[next(iter(n))] == 1 and v not in lists:
            out[v] = next(iter(n))
    # The generic handler of a format is the one most nodes declaring it use, all with that format, five times as
    # many as any other handler of that format only (sysctl_handle_quad: 262 "Q" nodes).
    users = collections.defaultdict(list)
    for handler, seen in formats.items():
        kinds = {HANDLERS.get(f) for f in seen}
        if len(kinds) == 1 and None not in kinds:
            users[kinds.pop()].append((sum(seen.values()), handler))
    for generic, counts in users.items():
        counts.sort(reverse=True)
        if counts[0][0] >= 50 and (len(counts) == 1 or counts[0][0] >= 5 * counts[1][0]):
            out[counts[0][1]] = generic
    return out
