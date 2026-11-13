import Darwin
import HWTestCore

/// A debug kernel's GM20B register dump (drivers/TegraGM20B/TegraGM20BRegDumpABI.h): the kernel reads the
/// registers L4T's capture reads and logs them as `tegra-gm20b: regdump v1` lines, which the log pull keeps. Each tag
/// dumps once a boot. The call makes no command buffer.
public enum RegDump {
    public enum Tag: UInt32 {
        case quiet = 0
        case hotStates = 1

        var name: String { self == .quiet ? "quiet" : "hot_states" }
    }

    static let method: UInt32 = 0x5244
    static let replyBytes = 40
    static let statuses = ["ok", "already", "not_ready", "no_memory"]
    static let engineBusy: UInt32 = 1 << 31

    /// regdump=<tag>:<ok|already|not_ready|no_memory|release_kernel|no_connection|...>; for a dump the kernel logged,
    /// regdump_badf= (registers that read a PRI error) and regdump_gr= (GR busy or idle before/after the reads).
    /// `connection` is opened beforehand by a caller that times the call against its GPU work.
    public static func capture(_ tag: Tag, on connection: GPUConnection? = GPUConnection()) -> Metrics {
        var metrics = Metrics()
        guard let gpu = connection else {
            metrics.set("regdump", tag.name + ":no_connection")
            return metrics
        }
        let (kr, reply) = gpu.call(method, GPUConnection.words([tag.rawValue, 0]), outputSize: replyBytes)
        guard kr == KERN_SUCCESS else {
            let why = kr == GPUConnection.kIOReturnUnsupported ? "release_kernel" : "err_" + GPUConnection.hex(kr)
            metrics.set("regdump", tag.name + ":" + why)
            return metrics
        }
        let status = Int(GPUConnection.int32(reply, 0))
        let answer = statuses.indices.contains(status) ? statuses[status] : "status\(status)"
        metrics.set("regdump", tag.name + ":" + answer)
        guard status == 0 else { return metrics }
        let gr = [GPUConnection.uint32(reply, 20), GPUConnection.uint32(reply, 24)].map {
            $0 & engineBusy != 0 ? "busy" : "idle"
        }
        metrics.set("regdump_badf", Int(GPUConnection.uint32(reply, 12)))
        metrics.set("regdump_gr", gr.joined(separator: "/"))
        return metrics
    }
}
