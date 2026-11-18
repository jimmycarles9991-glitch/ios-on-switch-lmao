import Darwin

/// A kernel sysctl the app reads, or why it could not: the sandbox denies the app's reads of some kern.* names (EPERM),
/// which is not the same finding as a kernel that lacks them (ENOENT).
enum KernelSysctl {
    enum Read<Value> {
        case value(Value)
        case failed(Int32)

        var error: Int32 {
            if case .failed(let error) = self { return error }
            return 0
        }
    }

    static func integer<T: FixedWidthInteger>(_ name: String, as type: T.Type) -> Read<T> {
        var value: T = 0
        var size = MemoryLayout<T>.size
        return sysctlbyname(name, &value, &size, nil, 0) == 0 ? .value(value) : .failed(errno)
    }

    static func string(_ name: String) -> Read<String> {
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0 else { return .failed(errno) }
        var bytes = [CChar](repeating: 0, count: max(size, 1))
        guard sysctlbyname(name, &bytes, &size, nil, 0) == 0 else { return .failed(errno) }
        return .value(String(cString: bytes))
    }

    /// "eperm", "enoent" or "errno<N>": the metric value of a failed read.
    static func code(_ error: Int32) -> String {
        switch error {
        case EPERM: return "eperm"
        case ENOENT: return "enoent"
        default: return "errno\(error)"
        }
    }

    /// The skip text for names the app could not read.
    static func unreadable(_ names: String, _ error: Int32) -> String {
        error == EPERM
            ? "the sandbox denies the app's read of \(names) (EPERM)"
            : error == ENOENT
                ? "the kernel has no \(names) (ENOENT: a release kernel)" : "\(names) unreadable (errno \(error))"
    }
}
