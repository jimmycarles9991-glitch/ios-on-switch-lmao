import Darwin
import Foundation
import HWTestGPU
import Metal

/// gpu.gm20b.reflection_kinds's wrong_kind arm: a binding asked another binding type's selector reports through Metal's
/// MTLReportFailure, type 0, and then answers a fixed value (c38-audit reflection-abort wrong_kind_probe.txt, an M4
/// Max; 21H461's and 24B119's Metal read the same). With no report block set Metal's default for type 0 aborts the
/// process: not an Objective-C exception, so nothing here could catch it. Each ask therefore runs with a block of the
/// test's own set around it alone (MTLSetReportFailureBlock, saved and set back), and a selector is never asked of
/// another type without one.
enum ReflectionWrongKind {
    typealias Report = @convention(block) (UInt64, UnsafePointer<CChar>?, Int32, NSString?) -> Void
    typealias SetBlock = @convention(c) (Report?) -> Void
    typealias GetBlock = @convention(c) () -> Report?
    typealias ErrorMode = @convention(c) (UInt64) -> UInt32
    typealias Enabled = @convention(c) (UInt64) -> Bool

    /// Each selector a binding not of `own`'s type answers through a report: its message, then its answer in
    /// ReflectionKinds.answer's encoding (nil and NO 0)
    struct Failure {
        let own: Int, then: Int, message: String
    }

    static let failures: [(String, Failure)] = [
        (
            "bufferAlignment",
            Failure(
                own: 0, then: 0,
                message: "Querying buffer alignment on an argument that is not a buffer"
            )
        ),
        (
            "bufferDataSize",
            Failure(
                own: 0, then: 0,
                message: "Querying buffer data size on an argument that is not a buffer"
            )
        ),
        (
            "bufferDataType",
            Failure(
                own: 0, then: 0,
                message: "Querying buffer data type on an argument that is not a buffer"
            )
        ),
        (
            "bufferStructType",
            Failure(
                own: 0, then: 0,
                message: "Querying buffer struct type on an argument that is not a buffer"
            )
        ),
        (
            "threadgroupMemoryAlignment",
            Failure(
                own: 1, then: 0,
                message: "Querying threadgroup memory alignment on an argument that is not a threadgroup memory"
            )
        ),
        (
            "threadgroupMemoryDataSize",
            Failure(
                own: 1, then: 0,
                message: "Querying threadgroup memory data size on an argument that is not a threadgroup memory"
            )
        ),
        (
            "textureType",
            Failure(
                own: 2, then: 2,
                message: "Querying texture type on an argument that is not a texture"
            )
        ),
        (
            "textureDataType",
            Failure(
                own: 2, then: 0,
                message: "Querying texture data type on an argument that is not a texture"
            )
        ),
        (
            "isDepthTexture",
            Failure(
                own: 2, then: 0,
                message: "Querying isDepthTexture on an argument that is not a texture"
            )
        ),
        (
            "objectPayloadAlignment",
            Failure(
                own: 34, then: 0,
                message: "Querying object payload alignment on an argument that is not an object payload"
            )
        ),
        (
            "objectPayloadDataSize",
            Failure(
                own: 34, then: 0,
                message: "Querying object payload size on an argument that is not an object payload"
            )
        ),
    ]

    struct Functions {
        let set: SetBlock, get: GetBlock, mode: ErrorMode, enabled: Enabled

        /// Metal's report-failure functions, both builds' Metal exporting them; nil where one is missing
        static func load() -> Functions? {
            let all = UnsafeMutableRawPointer(bitPattern: -2)
            guard let set = dlsym(all, "MTLSetReportFailureBlock"), let get = dlsym(all, "MTLGetReportFailureBlock"),
                  let mode = dlsym(all, "MTLFailureTypeGetErrorModeType"),
                  let enabled = dlsym(all, "MTLFailureTypeGetEnabled")
            else { return nil }
            return Functions(
                set: unsafeBitCast(set, to: SetBlock.self), get: unsafeBitCast(get, to: GetBlock.self),
                mode: unsafeBitCast(mode, to: ErrorMode.self), enabled: unsafeBitCast(enabled, to: Enabled.self)
            )
        }
    }

    struct Reported {
        let type: UInt64, function: String, message: String
    }

    /// The reports Metal made while `body` ran with a block of this test's set, and `body`'s answer
    static func reported(_ metal: Functions, _ body: () -> Int) -> (answer: Int, reports: [Reported]) {
        let lock = NSLock()
        var reports: [Reported] = []
        let saved = metal.get()
        metal.set { type, function, _, message in
            let report = Reported(
                type: type, function: function.map { String(cString: $0) } ?? "", message: (message as String?) ?? ""
            )
            lock.lock()
            reports.append(report)
            lock.unlock()
        }
        let answer = body()
        metal.set(saved)
        lock.lock()
        defer { lock.unlock() }
        return (answer, reports)
    }

    struct Counted {
        var asks = 0, reported = 0, off = 0, ownReports = 0
        var firstOff = "-"
    }

    /// The first binding of each type of each state asked every selector of `failures` not of its type (want one
    /// report of type 0, the message, a function ending " <selector>]", the answer after it) and each of its own
    /// type's (want none). Reports whose function names another selector are another thread's, not counted.
    static func asked(_ seen: [ReflectionKinds.Seen], _ metal: Functions) -> Counted {
        var out = Counted()
        var first: Set<String> = []
        func off(_ what: String) {
            out.off += 1
            if out.firstOff == "-" { out.firstOff = what }
        }
        for binding in seen {
            guard first.insert("\(binding.key.prefix { $0 != "." })/\(binding.type)").inserted else { continue }
            for (selector, failure) in failures {
                let (answer, reports) = reported(metal) { ReflectionKinds.answer(binding.object, selector) }
                let mine = reports.filter { $0.function.hasSuffix(" \(selector)]") }
                if failure.own == binding.type {
                    out.ownReports += mine.count
                    continue
                }
                out.asks += 1
                out.reported += mine.count
                let key = "\(binding.key).\(selector)"
                if mine.count != 1 {
                    off("\(key):reports_\(mine.count)")
                } else if UInt32(truncatingIfNeeded: mine[0].type) != 0 {
                    off("\(key):type_\(mine[0].type)")
                } else if mine[0].message != failure.message {
                    off("\(key):message")
                } else if answer != failure.then {
                    off("\(key):answer_\(answer)")
                }
            }
        }
        return out
    }
}
