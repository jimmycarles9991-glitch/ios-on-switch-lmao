import Darwin
import Foundation
import HWTestCore
import HWTestGPU

/// Compressed memory down to the GPU pool's floor for an app (drivers/TegraGM20B/TegraGM20BPoolPolicy.h
/// compressibleRefusal). Boot 22 refused 187 compressible BoAllocs, SwitchHWTest's first one at 49 MiB ready ("the
/// pool's last 48 MiB stay for coherent BOs and nvkm"). The app asks for compressible BOs of boot 22's 3840 KiB until
/// one is refused (at most 64), then for 2048 down to 128 KiB once each, and reads the pool's ready bytes through the
/// paths method's pool op (debug kernels) before and right after; every BO goes back at the end. PASS: the first
/// refusal is kIOReturnNoMemory, and the asks stopped at a hardware fact, within the last ask and 8 MiB (other
/// processes' and the scrubber's moves meanwhile): the floor (`ready_after_kib` < `floor_kib` + 128 + 8192), or no
/// whole clean 128 KiB chunk left in a fragmented pool (`clean_chunks_after` * 128 < 8192); something was made when
/// the pool had room. It drains the pool to its floor for a moment, so it runs after every other test (Registry.last):
/// a "compressed memory ... not made" line of any process inside its window is its own doing. Fields:
/// `ready_before_kib`, `ready_after_kib`, `floor_kib`, `held_before_kib`, `clean_chunks_before`, `clean_chunks_after`,
/// `made_kib`, `asks`, `refused` (the first refusal's kern_return_t).
struct CompressedFloorTest: HWTest {
    let id = "gpu.pool.compressed_floor"
    let name = "An app's compressed memory is made down to the GPU pool's floor"
    let subsystem = Subsystem.metal

    static let boAlloc: UInt32 = 2, boFree: UInt32 = 4, compressible: UInt32 = 1 << 2, alignLog2: UInt32 = 17
    static let pathsOpPool: UInt32 = 4
    static let firstKiB = 3840, maxFirstAsks = 64, slackKiB = 8192
    static let kIOReturnNoMemory = kern_return_t(bitPattern: 0xe000_02bd)

    func run(_ context: TestContext) async throws -> Outcome {
        guard let connection = GPUConnection() else { return .skip("no TegraGM20B connection") }
        guard let before = Self.pool(connection) else {
            return .fail("the kernel's paths method has no pool op (pool_op_unanswered)", Metrics())
        }
        var handles: [UInt64] = []
        var madeKiB = 0, asks = 0
        var refused: kern_return_t = KERN_SUCCESS
        func ask(_ kib: Int) -> Bool {
            asks += 1
            let bytes = UInt64(kib) << 10
            let input = GPUConnection.words([
                UInt32(truncatingIfNeeded: bytes), UInt32(bytes >> 32), Self.alignLog2, Self.compressible,
            ])
            let (kr, reply) = connection.call(Self.boAlloc, input, outputSize: 16)
            guard kr == KERN_SUCCESS else {
                if refused == KERN_SUCCESS { refused = kr }
                return false
            }
            handles.append(GPUConnection.uint64(reply, 0))
            madeKiB += kib
            return true
        }
        var first = 0
        while first < Self.maxFirstAsks && ask(Self.firstKiB) { first += 1 }
        for kib in [2048, 1024, 512, 256, 128] { _ = ask(kib) }
        let after = Self.pool(connection)
        for handle in handles {
            _ = connection.call(
                Self.boFree, GPUConnection.words([UInt32(truncatingIfNeeded: handle), UInt32(handle >> 32)]),
                outputSize: 0)
        }
        var metrics = Metrics()
        metrics.set("ready_before_kib", before["ready_kib"] ?? -1)
        metrics.set("ready_after_kib", after?["ready_kib"] ?? -1)
        metrics.set("floor_kib", before["floor_kib"] ?? -1)
        metrics.set("held_before_kib", before["held_kib"] ?? -1)
        metrics.set("clean_chunks_before", before["clean_chunks"] ?? -1)
        metrics.set("clean_chunks_after", after?["clean_chunks"] ?? -1)
        metrics.set("made_kib", madeKiB)
        metrics.set("asks", asks)
        metrics.set("refused", GPUConnection.hex(refused))
        let floor = before["floor_kib"] ?? 0
        let readyAfter = after?["ready_kib"] ?? Int.max
        let chunksAfter = after?["clean_chunks"] ?? Int.max / 128
        let atFloor = readyAfter < floor + 128 + Self.slackKiB
        let outOfChunks = chunksAfter * 128 < Self.slackKiB
        let roomBefore = (before["ready_kib"] ?? 0) >= floor + Self.firstKiB + Self.slackKiB
        let checks = [
            (
                "the first refusal is kIOReturnNoMemory (\(GPUConnection.hex(refused)))",
                refused == Self.kIOReturnNoMemory
            ),
            (
                "the asks stopped at the floor or the last clean chunk (\(readyAfter) KiB ready, floor \(floor) KiB, "
                    + "\(chunksAfter) chunks)",
                floor > 0 && (atFloor || outOfChunks)
            ),
            ("something was made while the pool had room (\(madeKiB) KiB)", !roomBefore || madeKiB > 0),
        ]
        return .judged(checks, metrics)
    }

    /// The paths method's pool op: "ready_kib=N floor_kib=N held_kib=N clean_chunks=N", or nil when the kernel does not
    /// answer it
    static func pool(_ connection: GPUConnection) -> [String: Int]? {
        let (kr, reply) = connection.call(GPUPaths.method, GPUConnection.words([pathsOpPool, 0]), outputSize: 512)
        guard kr == KERN_SUCCESS else { return nil }
        var fields: [String: Int] = [:]
        for field in String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self).split(separator: " ") {
            let pair = field.split(separator: "=", maxSplits: 1).map(String.init)
            if pair.count == 2, let value = Int(pair[1]) { fields[pair[0]] = value }
        }
        return fields["ready_kib"] == nil ? nil : fields
    }
}
