import Darwin
import Foundation
import HWTestCore
import HWTestGPU

/// A GPU job call the kernel refuses writes one error line naming why (drivers/TegraGM20B/SwitchGpuRefusalSites.h).
/// Boot 21 and 22 refused metal.native.counters' SubmitJob, and iconservicesagent's at 13:34:22.551, with no line at
/// all. On a connection of its own the app makes three calls the kernel refuses at three sites: a SubmitJob record of
/// zero bytes, a SubmitJob whose header names channel 0, and a ResourceSetCreate of no members. Each answers
/// kIOReturnBadArgument at once, and the connection's line count (the paths method's refusals op, debug kernels) moves
/// by three; the lines themselves read "tegra-gm20b: SwitchHWTest[pid]: SubmitJob refused, 0xe00002c2: the job
/// record's word is malformed ...", "... the job header is unreadable or names no channel ..." and "...
/// ResourceSetCreate refused, 0xe00002c2: the set record is malformed count=0 ...". Fields: `record_kr`, `header_kr`,
/// `set_kr`, `lines_before`, `lines_after`.
struct JobRefusalLinesTest: HWTest {
    let id = "gpu.job.refusal_lines"
    let name = "A GPU job call the kernel refuses writes one line naming the refusal"
    let subsystem = Subsystem.metal

    static let submitJob: UInt32 = 33, resourceSetCreate: UInt32 = 31, pathsOpRefusals: UInt32 = 5
    static let kIOReturnBadArgument = kern_return_t(bitPattern: 0xe000_02c2)

    func run(_ context: TestContext) async throws -> Outcome {
        guard let connection = GPUConnection() else { return .skip("no TegraGM20B connection") }
        guard let before = Self.lines(connection) else {
            return .fail("the kernel's paths method has no refusals op (refusals_op_unanswered)", Metrics())
        }
        let header = UnsafeMutableRawPointer.allocate(byteCount: 24, alignment: 8)
        defer { header.deallocate() }
        header.initializeMemory(as: UInt8.self, repeating: 0, count: 24)
        let address = UInt64(UInt(bitPattern: header))
        let split = { (value: UInt64) in [UInt32(truncatingIfNeeded: value), UInt32(value >> 32)] }
        let record = connection.call(Self.submitJob, GPUConnection.words(split(address) + [0, 0]), outputSize: 8).0
        let headed = connection.call(Self.submitJob, GPUConnection.words(split(address) + [24, 0]), outputSize: 8).0
        let set = connection.call(
            Self.resourceSetCreate, GPUConnection.words(split(address) + [0, 0]), outputSize: 16
        ).0
        let after = Self.lines(connection) ?? -1
        var metrics = Metrics()
        metrics.set("record_kr", GPUConnection.hex(record))
        metrics.set("header_kr", GPUConnection.hex(headed))
        metrics.set("set_kr", GPUConnection.hex(set))
        metrics.set("lines_before", before)
        metrics.set("lines_after", after)
        let checks = [
            (
                "each call is refused with kIOReturnBadArgument",
                [record, headed, set].allSatisfy { $0 == Self.kIOReturnBadArgument }
            ),
            ("three refusal lines (\(after - before))", after - before == 3),
        ]
        return .judged(checks, metrics)
    }

    /// The paths method's refusals op: "lines=N", or nil when the kernel does not answer it
    static func lines(_ connection: GPUConnection) -> Int? {
        let (kr, reply) = connection.call(
            GPUPaths.method, GPUConnection.words([pathsOpRefusals, 0]), outputSize: 512)
        guard kr == KERN_SUCCESS else { return nil }
        let text = String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self)
        guard text.hasPrefix("lines=") else { return nil }
        return Int(text.dropFirst(6))
    }
}
