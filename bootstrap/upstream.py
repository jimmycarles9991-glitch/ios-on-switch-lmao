from pathlib import Path

from bootstrap.xcode import replace_once


def adapt_ellekit(root: Path) -> None:
    adapt_ellekit_filters(root / 'injector/injector.c')
    adapt_ellekit_safety(root / 'injector/injector.c', root / 'sb/tweak.swift')
    adapt_ellekit_branches(root / 'ellekit')
    scheme = root / 'ellekit.xcodeproj/xcshareddata/xcschemes/safemode.xcscheme'
    text = scheme.read_text()
    if text.count('9E6FBB5829554734007B28FC') != 3:
        raise ValueError('ElleKit safe mode scheme differs from the source pin')
    scheme.write_text(
        text.replace('9E6FBB5829554734007B28FC', '9EA498A129565DC80099A3ED')
        .replace('BlueprintName = "safemode"', 'BlueprintName = "safemode-ui"')
        .replace('BuildableName = "safemode"', 'BuildableName = "libsafemode-ui.dylib"')
    )
    logger = root / 'logger/Logger.swift'
    text = logger.read_text()
    for declaration in (
        'public func dprint(',
        'public func tprint(',
        'public func print(\n    _ items:',
        'public func print(\n    _ item:',
        'private func log<T>(',
    ):
        if text.count(declaration) != 1:
            raise ValueError('ElleKit diagnostic declarations differ from the source pin')
        opening = text.index(') {', text.index(declaration)) + 2
        depth, end = 1, opening + 1
        while depth and end < len(text):
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        if depth:
            raise ValueError('unbalanced pinned ElleKit diagnostic function')
        text = text[: opening + 1] + '\n#if DEBUG' + text[opening + 1 : end - 1] + '#endif\n' + text[end - 1 :]
    logger.write_text(text)


def adapt_ellekit_branches(root: Path) -> None:
    branches = root / 'Assembler/Branches.swift'
    replace_once(
        branches,
        '    var imm = (opcode & 0x3FFFFFF) << 2\n'
        '    if (opcode & 0x2000000) == 1 {\n'
        '        // Sign extend\n'
        '        imm |= 0xFC000000\n'
        '    }\n'
        '    return Int(imm)\n'
        '}',
        '    Int(signExtend(UInt32(opcode & 0x3FFFFFF), 25)) * 4\n'
        '}\n\n'
        'func branchDestination(_ opcode: UInt64, pc: UInt64) -> UInt64 {\n'
        '    pc &+ UInt64(bitPattern: Int64(disassembleBranchImm(opcode)))\n'
        '}\n\n'
        'func encodableBranchOffset(_ offset: Int) -> Bool {\n'
        '    (-0x08000000 ..< 0x08000000).contains(offset) && offset.isMultiple(of: 4)\n'
        '}',
    )
    replace_once(
        branches,
        '    let pcRel = disassembleBranchImm(reverse(isn))\n\n'
        '    let originalTarget = UInt64(UInt(bitPattern: target)) + UInt64(pcRel)',
        '    let originalTarget = branchDestination(reverse(isn), pc: UInt64(UInt(bitPattern: target)))',
    )
    rebind = root / 'Assembler/Rebind.swift'
    replace_once(
        rebind,
        '                    var imm = UInt64(Int32((reversed & 0x3FFFFFF) << 2))\n'
        '                    if (reversed & 0x2000000) != 0 {\n'
        '                        // Sign extend\n'
        '                        imm |= 0xFFFFFFFFFC000000\n'
        '                    }\n\n'
        '                    imm += UInt64(4*index)',
        '                    let destination = branchDestination(UInt64(reversed), pc: formerPC + UInt64(4 * index))',
    )
    replace_once(rebind, 'formerPC &+ imm))', 'destination))')
    for link in ('true', 'false'):
        replace_once(
            rebind,
            f'assembleJump(formerPC &+ imm, pc: newPC, link: {link}',
            f'assembleJump(destination, pc: newPC, link: {link}',
        )
    replace_once(root / 'Languages/C/Orig.swift', 'if checkBranch(unpatched) {', 'if checkBranchUncond(isn) {')

    hook = root / 'Languages/C/Hook.swift'
    replace_once(
        hook,
        'targetSize >= 4 && abs(branchOffset / 1024 / 1024) > 128',
        'targetSize >= 4 && !encodableBranchOffset(branchOffset)',
    )
    replace_once(
        hook,
        'abs(branchOffset / 1024 / 1024) > 128 && enableEXCHooks',
        '!encodableBranchOffset(branchOffset) && enableEXCHooks',
    )
    replace_once(
        hook,
        '        print("[*] ellekit: Small branch")',
        '        guard encodableBranchOffset(branchOffset) else {\n'
        '            hooks.removeValue(forKey: target)\n'
        '            return nil\n'
        '        }\n'
        '        print("[*] ellekit: Small branch")',
    )


def adapt_ellekit_filters(path: Path) -> None:
    replace_once(path, '#include <dlfcn.h>', '#include <dlfcn.h>\n#include <math.h>')
    replace_once(
        path,
        '    if (access(path, F_OK) != 0) {',
        '    if (!path || access(path, F_OK) != 0) {',
    )
    replace_once(
        path,
        'kCFAllocatorDefault, path, strlen(path), false',
        'kCFAllocatorDefault, (const UInt8*)path, strlen(path), false',
    )
    replace_once(
        path,
        '    if (!ret) {\n        return false;\n    }',
        '    if (!ret || !data) {\n        if (data) CFRelease(data);\n        return false;\n    }',
    )
    replace_once(
        path,
        '    if (!plist) {',
        '    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {\n        if (plist) CFRelease(plist);',
    )
    replace_once(
        path,
        '    CFDictionaryRef filter = CFDictionaryGetValue(plist, CFSTR("Filter"));',
        '    CFDictionaryRef filter = CFDictionaryGetValue(plist, CFSTR("Filter"));\n'
        '    if (!filter || CFGetTypeID(filter) != CFDictionaryGetTypeID()) goto reject;',
    )
    replace_once(
        path,
        '    if (tweakManager != NULL) {\n        *isTweakManager = CFBooleanGetValue(tweakManager);\n    }',
        '    const bool is_manager = tweakManager && CFGetTypeID(tweakManager) == CFBooleanGetTypeID() &&\n'
        '        CFBooleanGetValue(tweakManager);',
    )
    text = path.read_text()
    start = '    CFArrayRef versions = CFDictionaryGetValue(filter, CFSTR("CoreFoundationVersion"));'
    end = '    CFArrayRef bundles = CFDictionaryGetValue(filter, CFSTR("Bundles"));'
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError('ElleKit version filter differs from the source pin')
    replace_once(
        path,
        text[text.index(start) : text.index(end)],
        start
        + """
    if (versions) {
        if (CFGetTypeID(versions) != CFArrayGetTypeID()) goto reject;
        CFIndex count = CFArrayGetCount(versions);
        if (count != 1 && count != 2) goto reject;
        for (CFIndex i = 0; i < count; i++) {
            CFTypeRef item = CFArrayGetValueAtIndex(versions, i);
            double version;
            if (!item || CFGetTypeID(item) != CFNumberGetTypeID() ||
                !CFNumberGetValue((CFNumberRef)item, kCFNumberDoubleType, &version) || !isfinite(version)) goto reject;
            if ((i == 0 && version > kCFCoreFoundationVersionNumber) ||
                (i == 1 && version <= kCFCoreFoundationVersionNumber)) goto reject;
        }
    }

""",
    )
    for key in ('bundles', 'classes', 'executables'):
        replace_once(path, f'    if ({key}) {{', f'    if ({key} && CFGetTypeID({key}) == CFArrayGetTypeID()) {{')
    replace_once(path, '            if (id) {', '            if (id && CFGetTypeID(id) == CFStringGetTypeID()) {')
    replace_once(
        path,
        '                CFStringLowercase(lowercased, NULL);',
        '                if (!lowercased) continue;\n                CFStringLowercase(lowercased, NULL);',
    )
    for key, indentation in (('classes', 12), ('executables', 16), ('executables', 24)):
        indent = ' ' * indentation
        declaration = f'\n{indent}CFStringRef id = CFArrayGetValueAtIndex({key}, i);'
        replace_once(
            path, declaration, declaration + f'\n{indent}if (!id || CFGetTypeID(id) != CFStringGetTypeID()) continue;'
        )
    for indent in (' ' * 16, ' ' * 20):
        copy = 'CFStringGetCString(id, copiedStr, CFStringGetLength(id) + 1, kCFStringEncodingASCII);'
        replace_once(
            path,
            '\n' + indent + copy,
            f'\n{indent}if (!copiedStr) continue;\n'
            f'{indent}if (!{copy[:-1]}) {{\n'
            f'{indent}    free(copiedStr);\n'
            f'{indent}    continue;\n'
            f'{indent}}}',
        )
    replace_once(
        path,
        '        if (_NSGetExecutablePath(executable, &size) == 0) {',
        '        if (_NSGetExecutablePath(executable, &size) == 0) {\n'
        '            const char* name = get_last_path_component(executable);\n'
        '            if (!name) name = executable;',
    )
    for value in ('str', 'copiedStr'):
        replace_once(path, f'strcmp({value}, get_last_path_component(executable))', f'strcmp({value}, name)')
    replace_once(
        path,
        '    CFRelease(plist);\n    return false;\n\nsuccess:\n    CFRelease(plist);',
        'reject:\n    CFRelease(plist);\n    return false;\n\nsuccess:\n'
        '    *isTweakManager = is_manager;\n    CFRelease(plist);',
    )
    replace_once(
        path,
        '            char* plist = strndup(full_path, strlen(full_path) - 6);',
        '            char* plist = full_path ? strndup(full_path, strlen(full_path) - 6) : NULL;',
    )
    replace_once(
        path,
        'bool ret = tweak_needinject(plist, &isTweakManager);',
        'bool ret = plist && tweak_needinject(plist, &isTweakManager);',
    )
    replace_once(
        path,
        '                    if (i == MAX_TWEAKMANAGERS) return;                // array is full\n'
        '                    tweakManagers[i] = malloc(strlen(full_path) + 1);  // allocate memory for the new string\n'
        '                    strcpy(tweakManagers[i], full_path);'
        '               // copy the string into the new memory location',
        '                    if (i < MAX_TWEAKMANAGERS) tweakManagers[i] = strdup(full_path);',
    )


def adapt_ellekit_safety(injector: Path, ui: Path) -> None:
    replace_once(injector, '#include <dlfcn.h>', '#include <dlfcn.h>\n#include <fcntl.h>\n#include <signal.h>')
    entry = '__attribute__((constructor)) static void injection_init(void) {'
    replace_once(
        injector,
        entry,
        """#if !TARGET_OS_OSX
static const char switch_safety_marker[] = "/var/mobile/.eksafemode";

static void switch_safety_crash(int number) {
    int fd = open(switch_safety_marker, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) close(fd);
    signal(number, SIG_DFL);
    raise(number);
}

static bool switch_safety_arm(void) {
    int fd = open(switch_safety_marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    int removed = unlink(switch_safety_marker);
    close(fd);
    if (removed) return false;

    static const int signals[] = { SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGEMT, SIGFPE, SIGBUS, SIGSEGV, SIGSYS };
    struct sigaction action = {0};
    action.sa_handler = switch_safety_crash;
    if (sigemptyset(&action.sa_mask)) return false;
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); i++) {
        if (sigaction(signals[i], &action, NULL)) return false;
    }
    return true;
}
#endif

"""
        + entry,
    )
    text = injector.read_text()
    start = '    if (CFBundleGetMainBundle() && CFBundleGetIdentifier(CFBundleGetMainBundle())) {'
    end = '    const char* extension = getenv("SANDBOX_EXTENSION");'
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError('ElleKit MobileSafety startup differs from the source pin')
    replace_once(
        injector,
        text[text.index(start) : text.index(end)],
        """    CFBundleRef bundle = CFBundleGetMainBundle();
    CFStringRef identifier = bundle ? CFBundleGetIdentifier(bundle) : NULL;
    const bool springboard = identifier && CFEqual(identifier, CFSTR("com.apple.springboard"));
    if (springboard) {
        const char* safety = rootless ? MOBILESAFETY_PATH_ROOTLESS : MOBILESAFETY_PATH_ROOTFUL;
        if (!dlopen(safety, RTLD_NOW)) {
            const char* error = dlerror();
            os_log_error(OS_LOG_DEFAULT, "ElleKit MobileSafety failed to load: %{public}s",
                         error ? error : "unknown error");
            return;
        }
    }
    if (access(switch_safety_marker, F_OK) == 0) return;
    if (springboard && !switch_safety_arm()) {
        os_log_error(OS_LOG_DEFAULT, "ElleKit could not arm SpringBoard safe mode");
        return;
    }
#endif

""",
    )
    text = ui.read_text()
    for start, end in (
        ('let crashSignals = [', 'let exitSafeModeSignals = ['),
        ('func handleSBCrash(currentSig: Int32) {', '@_cdecl("tweak_entry")'),
    ):
        if text.count(start) != 1 or text.count(end) != 1:
            raise ValueError('ElleKit MobileSafety handlers differ from the source pin')
        replace_once(ui, text[text.index(start) : text.index(end)], '')
    replace_once(ui, '    trap(signals: crashSignals, action: handleSBCrash)\n', '')
