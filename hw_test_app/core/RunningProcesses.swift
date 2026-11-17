import Darwin

/// The processes running now, as the kern.proc.all sysctl lists them. The app's sandbox refuses that read unless the
/// name is in its sysctl exceptions (boot 22: "deny(1) sysctl-read kern.proc.all.0" for the four-part name, which
/// carries KERN_PROC_ALL's unused argument); asked by name, it is the three-part "kern.proc.all" the exceptions list.
/// A list that shows no process but the caller's own proves nothing about the others (LaunchTarget).
public enum RunningProcesses {
    public struct Entry: Equatable, Sendable {
        public let pid: Int32
        public let name: String

        public init(pid: Int32, name: String) {
            self.pid = pid
            self.name = name
        }
    }

    /// Every process the list shows, by pid and command name; nil when the read is refused. `name` is the sysctl,
    /// which the caller names so tools/devicerun/hwtest_test.py finds it in the app's sandbox exceptions.
    public static func list(sysctl name: String) -> [Entry]? {
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var procs = [kinfo_proc](repeating: kinfo_proc(), count: size / MemoryLayout<kinfo_proc>.stride + 8)
        size = procs.count * MemoryLayout<kinfo_proc>.stride
        guard sysctlbyname(name, &procs, &size, nil, 0) == 0 else { return nil }
        return procs.prefix(size / MemoryLayout<kinfo_proc>.stride).map { proc in
            var comm = proc.kp_proc.p_comm
            let name = withUnsafeBytes(of: &comm) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
            return Entry(pid: proc.kp_proc.p_pid, name: name)
        }
    }

    /// The first pid in `list` whose command name is `name`; nil when it shows none or the list was refused
    public static func pid(named name: String, in list: [Entry]?) -> Int32? {
        list?.first { $0.name == name }?.pid
    }
}

/// Whether ui.launch_hitch's target app ran when the test began, as far as the process list can tell
public enum LaunchTarget: String, Sendable {
    case running
    case notRunning = "not_running"
    case unknown

    /// `list` from RunningProcesses.list(), `own` the caller's pid: a list that shows no process but the caller's own
    /// cannot tell a missing target from a hidden one
    public static func state(of process: String, in list: [RunningProcesses.Entry]?, own: Int32) -> LaunchTarget {
        guard let list, list.contains(where: { $0.pid != own }) else { return .unknown }
        return list.contains { $0.name == process } ? .running : .notRunning
    }

    /// The test's rounds: a cold one only where the target is known not to run, then a warm one; where it is not
    /// known, the first round is reported as "first", neither cold nor warm
    public var arms: [String] {
        switch self {
        case .notRunning: return ["cold", "warm"]
        case .running: return ["warm"]
        case .unknown: return ["first", "warm"]
        }
    }
}
