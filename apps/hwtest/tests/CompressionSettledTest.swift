import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The compression thread's debug event (CompressionSettled, gpu.h compWhenSettled): a private BGRA8 target of a
/// size no other test makes asks for compressed memory at its creation; the event's block runs once that memory is made
/// or refused (settled, settled_calls 1: once, never twice), and the same target made after it binds compressed memory
/// (comptags 1). Asked again with nothing wanted, the block runs on the caller's thread before the call returns
/// (idle_sync 1). first_comptags (where kept memory already served the first target, nothing was asked: the event then
/// settles at once), asked_sync (the first block ran before its call returned), made and refused (switchMetalCompression
/// over the test) and settled_ms are reported.
struct CompressionSettledTest: HWTest {
    let id = "gpu.gm20b.compression_settled"
    let name = "The compression thread's settled event runs once after an ask and at once with none"
    let subsystem = Subsystem.metal

    static let width = 1216, height = 688

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard let facade = gpu.device as? NSObject, facade.responds(to: CompressionSettled.selector) else {
            return .skip(GM20BFacade.missing("switchMetalProbeCompressionSettled:", on: gpu.device))
        }
        let before = GM20BFacade.compression(of: gpu.device)
        guard !before.isEmpty else { return .skip(GM20BFacade.missing("switchMetalCompression", on: gpu.device)) }
        let make = { try gpu.texture(.bgra8Unorm, Self.width, Self.height) }
        let first = try make()
        let firstComptags = CompressionSettled.comptags(gpu.device, first) ?? -1
        let calls = CallCount()
        let waiter = Waiter<Double>()
        let start = CFAbsoluteTimeGetCurrent()
        let refusal = CompressionSettled.ask(facade) {
            calls.add()
            waiter.resume((CFAbsoluteTimeGetCurrent() - start) * 1000)
        }
        let askedSync = calls.count > 0
        let settledMs = refusal == nil ? await waiter.wait(timeout: CompressionSettled.bound) : nil
        let second = try make()
        let comptags = CompressionSettled.comptags(gpu.device, second) ?? -1
        let idle = CallCount()
        _ = CompressionSettled.ask(facade) { idle.add() }
        let idleSync = idle.count == 1
        let after = GM20BFacade.compression(of: gpu.device)
        var metrics = Metrics()
        metrics.set("settled", settledMs != nil)
        metrics.set("settled_calls", calls.count)
        metrics.set("comptags", comptags)
        metrics.set("idle_sync", idleSync)
        if let settledMs { metrics.set("settled_ms", settledMs, digits: 3) }
        metrics.set("first_comptags", firstComptags)
        metrics.set("asked_sync", askedSync)
        metrics.set("made", (after["made"] ?? 0) - (before["made"] ?? 0))
        metrics.set("refused", (after["refused"] ?? 0) - (before["refused"] ?? 0))
        withExtendedLifetime(first) {}
        return .judged(
            [
                ("the settled block ran within \(Int(CompressionSettled.bound)) s", settledMs != nil),
                ("the settled block ran once (\(calls.count))", calls.count == 1),
                ("the target made after it binds compressed memory (comptags \(comptags))", comptags == 1),
                ("with nothing asked the block ran before the call returned", idleSync),
            ], metrics, proof: gpu.proof
        )
    }
}

private final class CallCount: @unchecked Sendable {
    private let lock = NSLock()
    private var calls = 0

    func add() {
        lock.lock()
        calls += 1
        lock.unlock()
    }

    var count: Int {
        lock.lock()
        defer { lock.unlock() }
        return calls
    }
}
