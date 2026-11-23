import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A private target whose compressed memory the kernel refused for a shortage asks again when this process gives some
/// back, and moves onto it (drivers/TegraGM20BMetal/private_comp.cpp compGaveBack). A private
/// RGBA16Float 640x480 target on compressed memory (C64) is held; the GPU pool is filled to its floor by compressible
/// BoAllocs of a connection of the test's own (CompressedFloorTest's asks); a private BGRA8 1792x1472 target is made:
/// its ask is refused (`refused` >= 1 once the compression thread settled, `comptags_at_creation` 0). The connection's
/// memory goes back, which is another client's give-back: nothing is asked again (`made_before_release` 0). Then the
/// held RGBA16Float target goes: its memory, of another kind, serves no refusal, so it is given back and the refused
/// ask is made (`made_after_giveback` >= 1, the thread settled again); two command buffers of an empty pass later the
/// target is on compressed memory (`comptags_after` 1, `moves` 1). Judged only where the target was made plain (a kept
/// memory of its kind at least as large serves it at creation: `giveback_unjudged` 1). Red: comptags_after 0 with
/// made_after_giveback 0 (compGaveBack asked nothing: the refused ask's bytes differ from the want's compBytes); made
/// but comptags 0 (the move's due flag or the worker's look). It drains the pool to its floor for a moment, refusing
/// every other process's compressed asks meanwhile: it runs among the last tests (Registry.last).
struct CompressionGivebackTest: HWTest {
    let id = "gpu.gm20b.compression_giveback"
    let name = "A refused ask for compressed memory is made again when this process gives memory back"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let width = 1792, height = 1472

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard (gpu.device as? NSObject)?.responds(to: CompressionSettled.factsSelector) == true else {
            return .skip(GM20BFacade.missing("switchMetalProbeFacts:", on: gpu.device))
        }
        guard let connection = GPUConnection() else { return .skip("no TegraGM20B connection") }
        var metrics = Metrics()
        var held: MTLTexture? = try await CompressionSettled.remade(gpu.device) {
            try gpu.texture(.rgba16Float, 640, 480, usage: [.renderTarget, .shaderRead])
        }
        let heldCompressed = CompressionSettled.comptags(gpu.device, held!) ?? -1
        metrics.set("held_comptags", heldCompressed)
        guard heldCompressed == 1 else { return .fail("no compressed RGBA16Float target to give back", metrics) }
        var handles = Self.fill(connection)
        defer { Self.free(connection, handles) }
        metrics.set("filled_kib", handles.reduce(0) { $0 + $1.kib })
        let before = GM20BFacade.compression(of: gpu.device)
        let movesBefore = GM20BFacade.counters(of: gpu.device)["private_compressed_moves"] ?? 0
        let target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let atCreation = CompressionSettled.comptags(gpu.device, target) ?? -1
        try await CompressionSettled.settle(gpu.device)
        let refused = GM20BFacade.compression(of: gpu.device)
        Self.free(connection, handles)
        handles = []
        let beforeRelease = GM20BFacade.compression(of: gpu.device)
        autoreleasepool { held = nil }
        try await CompressionSettled.settle(gpu.device)
        let gaveBack = GM20BFacade.compression(of: gpu.device)
        let scratch = try gpu.texture(.bgra8Unorm, 16, 16, usage: [.renderTarget, .pixelFormatView])
        for _ in 0 ..< 2 {
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = scratch
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
        }
        let after = CompressionSettled.comptags(gpu.device, target) ?? -1
        let moves = (GM20BFacade.counters(of: gpu.device)["private_compressed_moves"] ?? 0) - movesBefore
        let delta = { (a: [String: Int], b: [String: Int], key: String) in (b[key] ?? 0) - (a[key] ?? 0) }
        metrics.set("comptags_at_creation", atCreation)
        metrics.set("refused", delta(before, refused, "refused"))
        metrics.set("made_before_release", delta(before, beforeRelease, "made"))
        metrics.set("made_after_giveback", delta(beforeRelease, gaveBack, "made"))
        metrics.set("comptags_after", after)
        metrics.set("moves", moves)
        guard atCreation == 0 else {
            metrics.set("giveback_unjudged", 1)
            return Outcome.pass(metrics, proof: gpu.proof).onRawPath(gpu)
        }
        return Outcome.judged(
            [
                ("the kernel refused the target's compressed memory", delta(before, refused, "refused") >= 1),
                (
                    "nothing made between the refusal and this process's give-back",
                    delta(before, beforeRelease, "made") == 0
                ),
                ("the refused ask made again after the give-back", delta(beforeRelease, gaveBack, "made") >= 1),
                ("the target on compressed memory after it (comptags \(after))", after == 1),
                ("moved once (\(moves))", moves == 1),
            ], metrics, proof: gpu.proof
        ).onRawPath(gpu)
    }

    /// Compressible BoAllocs of CompressedFloorTest's sizes until the kernel refuses one, then the smaller sizes once
    /// each
    static func fill(_ connection: GPUConnection) -> [(handle: UInt64, kib: Int)] {
        var handles: [(handle: UInt64, kib: Int)] = []
        func ask(_ kib: Int) -> Bool {
            let bytes = UInt64(kib) << 10
            let input = GPUConnection.words([
                UInt32(truncatingIfNeeded: bytes), UInt32(bytes >> 32), CompressedFloorTest.alignLog2,
                CompressedFloorTest.compressible,
            ])
            let (kr, reply) = connection.call(CompressedFloorTest.boAlloc, input, outputSize: 16)
            guard kr == KERN_SUCCESS else { return false }
            handles.append((GPUConnection.uint64(reply, 0), kib))
            return true
        }
        var first = 0
        while first < CompressedFloorTest.maxFirstAsks && ask(CompressedFloorTest.firstKiB) { first += 1 }
        for kib in [2048, 1024, 512, 256, 128] { _ = ask(kib) }
        return handles
    }

    static func free(_ connection: GPUConnection, _ handles: [(handle: UInt64, kib: Int)]) {
        for (handle, _) in handles {
            _ = connection.call(
                CompressedFloorTest.boFree,
                GPUConnection.words([UInt32(truncatingIfNeeded: handle), UInt32(handle >> 32)]), outputSize: 0
            )
        }
    }
}
