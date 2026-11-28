import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Texture moves run on the submit worker, which gives back an open encode ahead first (private_comp.h; the
/// facade's freshBeforeTransfer). Core Animation's scale frame (CommitLatencyPathTest's encode: 15 passes of 6 draws
/// over 4 targets, a pass every 400 us, so the worker encodes ahead) is recorded `frames` times, then again while a
/// second thread, at each pass, makes a fresh private texture and writes it with replaceRegion (a move). Every frame
/// completes; the targets read back as in the run with no mover (targets_differ 0); pipeline_abandons.other_work rises
/// by at most the moves made (each move that meets an open encode ahead gives it back first). Reported: moves,
/// other_work_abandons, device_abandons.
struct AheadMovesTest: HWTest {
    let id = "gpu.path.ahead_moves"
    let name = "Texture moves beside a frame encoded ahead give it back first and leave its pixels as they were"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let frames = 6, passes = 15, drawsPerPass = 6, moveSide = 64

    final class Mover: @unchecked Sendable {
        private let lock = NSLock()
        private var running = true
        private(set) var made = 0
        private let asked = DispatchSemaphore(value: 0), finished = DispatchSemaphore(value: 0)

        func start(_ device: MTLDevice) {
            let side = AheadMovesTest.moveSide
            let bytes = [UInt8](repeating: 0x3c, count: side * side * 4)
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm, width: side, height: side, mipmapped: false
            )
            d.storageMode = .private
            d.usage = [.shaderRead]
            Thread { [self] in
                while true {
                    asked.wait()
                    lock.lock()
                    let go = running
                    lock.unlock()
                    guard go, let texture = device.makeTexture(descriptor: d) else { break }
                    bytes.withUnsafeBytes {
                        texture.replace(
                            region: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 0, withBytes: $0.baseAddress!,
                            bytesPerRow: side * 4
                        )
                    }
                    lock.lock()
                    made += 1
                    lock.unlock()
                }
                finished.signal()
            }.start()
        }

        func ask() { asked.signal() }

        func stop() -> Int {
            lock.lock()
            running = false
            lock.unlock()
            asked.signal()
            finished.wait()
            return made
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            return .skip("no QuartzCore library here (\(GPU.describe(error)))")
        }
        guard let line = CACorpus.state("PBGRAXm_A2Xhf") else { return .fail("CACorpus has no PBGRAXm_A2Xhf") }
        let pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
        let side = CommitLatencyPathTest.width
        let scratch = try gpu.texture(
            CACorpus.colorFormat(line, 1), side, side, usage: [.renderTarget], storage: .memoryless
        )
        var metrics = Metrics()
        var failures: [String] = []
        func frames(_ mover: Mover?) async throws -> [[UInt8]] {
            let targets = try (0 ..< CommitLatencyPathTest.targets)
                .map { _ in try gpu.texture(.bgra8Unorm, side, side) }
            for round in 0 ..< Self.frames {
                guard let commands = gpu.queue.makeCommandBuffer() else { throw TestFailure("no command buffer") }
                for pass in 0 ..< Self.passes {
                    let target = targets[pass % targets.count]
                    CommitLatencyPathTest.encodePass(
                        commands, pipeline: pipeline, target: target, scratch: scratch, pass: pass, round: round,
                        clear: pass < targets.count, draws: Self.drawsPerPass
                    )
                    mover?.ask()
                    usleep(CommitLatencyPathTest.paceMicroseconds)
                }
                commands.commit()
                do {
                    try gpu.finish([commands])
                } catch {
                    failures.append("frame \(round): \(GPU.describe(error))")
                }
            }
            var bytes: [[UInt8]] = []
            for target in targets { bytes.append(try await gpu.read(target, bytesPerPixel: 4)) }
            return bytes
        }
        let alone = try await frames(nil)
        let before = GM20BFacade.counters(of: gpu.device)
        let mover = Mover()
        mover.start(gpu.device)
        let beside = try await frames(mover)
        let moves = mover.stop()
        let after = GM20BFacade.counters(of: gpu.device)
        let delta = { (key: String) in (after[key] ?? 0) - (before[key] ?? 0) }
        let differ = zip(alone, beside).map { zip($0, $1).filter { $0 != $1 }.count }.reduce(0, +)
        let otherWork = delta("pipeline_abandons.other_work")
        metrics.set("moves", moves)
        metrics.set("targets_differ", differ)
        metrics.set("other_work_abandons", otherWork)
        metrics.set("device_abandons", delta("pipeline_abandons.device"))
        if moves == 0 { failures.append("the mover made no texture") }
        if differ != 0 { failures.append("\(differ) target bytes differ from the frames with no mover") }
        if otherWork > moves { failures.append("\(otherWork) encodes ahead given back for other work, \(moves) moves") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
