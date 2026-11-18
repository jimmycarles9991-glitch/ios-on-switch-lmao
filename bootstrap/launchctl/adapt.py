import re
import sys
from pathlib import Path


def adapt_userswitch(source: str) -> str:
    start = source.index('\tint ret = ENOTSUP;')
    end = source.index('\n\tif (ret != 0)', start)
    return (
        source[:start].replace('#include <errno.h>', '#include <dlfcn.h>\n#include <errno.h>')
        + '\tint ret = ENOTSUP;\n'
        + '\tint (*switch_users)(long, long) = (int (*)(long, long))dlsym(RTLD_DEFAULT, "launch_active_user_switch");\n'
        + '\tif (switch_users)\n'
        + '\t\tret = switch_users(strtol(argv[1], NULL, 0), strtol(argv[2], NULL, 0));\n'
        + source[end:]
    )


def adapt_procinfo(source: str, donor_header: str) -> str:
    layout = re.search(r'^struct proc_bsdinfo \{.*?^\};', donor_header, re.MULTILINE | re.DOTALL)
    flags = set(re.findall(r'PROC_(?:FLAG|DIRTY)_\w+', source))
    flag_pattern = r'^#define[^\S\n]+(PROC_(?:FLAG|DIRTY)_\w+)[^\S\n]+(\w+)'
    definitions = {name: value for name, value in re.findall(flag_pattern, donor_header, re.MULTILINE) if name in flags}
    if not layout or flags != definitions.keys():
        raise ValueError('the selected XNU process information ABI is incomplete')
    source = source.replace(
        '#include <libproc.h>',
        '#include <sys/param.h>\n'
        'int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);\n'
        'int proc_pidpath(int pid, void *buffer, uint32_t buffersize);\n'
        'int proc_get_dirty(pid_t pid, uint32_t *flags);',
    )
    abi = layout[0] + '\n' + '\n'.join(f'#define {name} {value}' for name, value in definitions.items())
    return source.replace('#include <sys/proc_info.h>', abi)


def adapt_bootstrap(source: str) -> str:
    source = source.replace('#include <errno.h>', '#include "bootstrap/launchctl/daemon_result.h"\n#include <errno.h>')
    start = '\tif (argc < 2)\n\t\treturn EUSAGE;\n'
    if source.count(start) != 2:
        raise ValueError('the pinned launchctl bootstrap entry changed')
    source = source.replace(
        start,
        start
        + '\tconst bool switch_result = strcmp(argv[1], "--switch-bootstrap-result") == 0;\n'
        + '\tif (switch_result) {\n'
        + '\t\tif (argc != 4 || strcmp(argv[2], "system") || argv[3][0] != \'/\')\n'
        + '\t\t\treturn EUSAGE;\n'
        + '\t\targc--;\n\t\targv++;\n\t}\n',
    )
    for routine, removing in (('LOAD', 'false'), ('UNLOAD', 'true')):
        invocation = f'\tret = launchctl_send_xpc_to_launchd(XPC_ROUTINE_{routine}, dict, &reply);'
        if source.count(invocation) != 1:
            raise ValueError('the pinned launchctl native routine changed')
        source = source.replace(
            invocation,
            invocation
            + '\n\tif (switch_result) {\n'
            + f'\t\tconst int result = switch_launchctl_daemon_result(ret, ret ? NULL : reply, {removing});\n'
            + '\t\tif (result == SWITCH_LAUNCHCTL_FAILED)\n'
            + f'\t\t\tswitch_launchctl_daemon_diagnostic(ret, ret ? NULL : reply, {removing});\n'
            + '\t\treturn result;\n\t}',
        )
    return source


if __name__ == '__main__':
    variant, *paths = sys.argv[1:]
    source, target, *inputs = map(Path, paths)
    if variant == 'userswitch':
        target.write_text(adapt_userswitch(source.read_text()))
    elif variant == 'procinfo':
        target.write_text(adapt_procinfo(source.read_text(), inputs[0].read_text()))
    elif variant == 'bootstrap':
        target.write_text(adapt_bootstrap(source.read_text()))
    else:
        raise ValueError(f'unknown launchctl source: {variant}')
