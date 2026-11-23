import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A closed depth texture's compressed memory serves the next one of its kind at its creation
/// (drivers/TegraGM20BMetal/private_comp.cpp compRelease: kept made where that holds back no ask
/// refused for a shortage). A private Depth32Float 1024x600 made, the compression thread settled (CompressionSettled),
/// the first let go and a second made (`second_comptags_at_creation` 1); the second let go with nothing submitted
/// meanwhile, and a third made at once: `third_comptags_at_creation` 1, no compressed memory made for it (`made_delta`
/// 0) and the bytes the facade holds the same across the release and the make (`held_bytes_delta` 0). Each make is
/// timed (`second_make_us`, `third_make_us`, reported: both bind memory already made). Red: compRelease gave the memory
/// back (held_bytes falls, the third is plain or made anew), or servesRefusals held back what it should keep.
struct ZetaKeptTest: HWTest {
    let id = "gpu.gm20b.zeta_kept"
    let name = "A closed depth texture's compressed memory serves the next of its kind at creation"
    let subsystem = Subsystem.metal

    static let width = 1024, height = 600

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard (gpu.device as? NSObject)?.responds(to: CompressionSettled.factsSelector) == true else {
            return .skip(GM20BFacade.missing("switchMetalProbeFacts:", on: gpu.device))
        }
        let make = { try gpu.texture(.depth32Float, Self.width, Self.height, usage: [.renderTarget, .shaderRead]) }
        var first: MTLTexture? = try make()
        let firstAtCreation = CompressionSettled.comptags(gpu.device, first!) ?? -1
        try await CompressionSettled.settle(gpu.device)
        first = nil
        let start = CFAbsoluteTimeGetCurrent()
        var second: MTLTexture? = try make()
        let secondUs = (CFAbsoluteTimeGetCurrent() - start) * 1_000_000
        let secondAtCreation = CompressionSettled.comptags(gpu.device, second!) ?? -1
        let held = GM20BFacade.compression(of: gpu.device)
        autoreleasepool { second = nil }
        let released = GM20BFacade.compression(of: gpu.device)
        let (third, thirdUs) = try Self.timed(make)
        let made = GM20BFacade.compression(of: gpu.device)
        let thirdAtCreation = CompressionSettled.comptags(gpu.device, third) ?? -1
        let heldDelta = (made["held_bytes"] ?? 0) - (held["held_bytes"] ?? 0)
        let releaseDelta = (released["held_bytes"] ?? 0) - (held["held_bytes"] ?? 0)
        let madeDelta = (made["made"] ?? 0) - (held["made"] ?? 0)
        var metrics = Metrics()
        metrics.set("first_comptags_at_creation", firstAtCreation)
        metrics.set("second_comptags_at_creation", secondAtCreation)
        metrics.set("third_comptags_at_creation", thirdAtCreation)
        metrics.set("made_delta", madeDelta)
        metrics.set("held_bytes_delta", heldDelta)
        metrics.set("released_bytes_delta", releaseDelta)
        metrics.set("second_make_us", secondUs, digits: 1)
        metrics.set("third_make_us", thirdUs, digits: 1)
        return .judged(
            [
                ("the second was made on compressed memory (comptags \(secondAtCreation))", secondAtCreation == 1),
                ("the third was made on compressed memory (comptags \(thirdAtCreation))", thirdAtCreation == 1),
                ("no compressed memory made for the third (\(madeDelta))", madeDelta == 0),
                ("the bytes held stayed across the release (\(releaseDelta))", releaseDelta == 0),
                ("and across the third's make (\(heldDelta))", heldDelta == 0),
            ], metrics, proof: gpu.proof
        )
    }

    static func timed(_ make: () throws -> MTLTexture) throws -> (MTLTexture, Double) {
        let start = CFAbsoluteTimeGetCurrent()
        let texture = try make()
        return (texture, (CFAbsoluteTimeGetCurrent() - start) * 1_000_000)
    }
}
