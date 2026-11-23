import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A Depth16Unorm a fetching draw tests in its shader, in a layered pass and in a pass into a turned plane
/// (Depth16LayeredCases), each rendered through the driver's converted Depth32Float_Stencil8 (TegraGM20BMetalRangeForms.mm
/// depth16Tested). Expected values are an M4 Max's (this scene, measured): layer 0 keeps its clear's code 16383
/// everywhere (0.5 < 0.25 fails); layers 1 and 2 hold the draw's 0.5, 32768 (the spec allows one code either way),
/// inside the rectangle and their clears' 36044 and 55704 outside it (Metal's floor of the clear, Depth16ClearTest);
/// the colour is written inside the rectangle of layers 1 and 2 alone. d16_layer<l>_code is the code at the
/// rectangle's centre, d16_layer<l>_off the texels off those, d16_colour_off the colour texels off. Turned: the depth the
/// pass into the turned plane stores equals the plain pass's texel for texel, and the plane holds the plain colour
/// turned by Metal's store map, as on an M4 Max (d16_turned_match; _depth_off and _colour_off the texels apart; a device
/// with no rotation SPI FAILs with d16_turned_match=-).
struct Depth16LayeredTestsTest: HWTest {
    let id = "gpu.gm20b.depth16_layered_tests"
    let name = "A Depth16Unorm a fetching draw tests in its shader, layered and turned, stores Metal's codes, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let clearCodes: [UInt16] = [16383, 36044, 55704]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        return await Self.outcome(gpu)
    }

    static func outcome(_ gpu: GPU) async -> Outcome {
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let r = try await Depth16LayeredCases.layered(gpu)
            let centre =
                (Depth16LayeredCases.rect.y0 + Depth16LayeredCases.rect.y1) / 2 * Depth16LayeredCases.side
                    + (Depth16LayeredCases.rect.x0 + Depth16LayeredCases.rect.x1) / 2
            for l in 0 ..< Depth16LayeredCases.layers {
                let off = Depth16LayeredCases.depthOff(r, layer: l, clear: clearCodes[l])
                metrics.set("d16_layer\(l)_code", Int(r.depth[l][centre]))
                metrics.set("d16_layer\(l)_off", off)
                checks.append(("layer \(l): Metal's depth codes", off == 0))
            }
            let colourOff = Depth16LayeredCases.colourOff(r)
            metrics.set("d16_colour_off", colourOff)
            checks.append(("colour written in layers 1 and 2 alone", colourOff == 0))
        } catch {
            checks.append(("layered: \(GPU.describe(error))", false))
        }
        do {
            if let t = try await Depth16LayeredCases.turned(gpu) {
                let match = t.depthOff == 0 && t.colourOff == 0
                metrics.set("d16_turned_match", match)
                metrics.set("d16_turned_depth_off", t.depthOff)
                metrics.set("d16_turned_colour_off", t.colourOff)
                checks.append(("turned: the plain pass's depth and turned colour", match))
            } else {
                metrics.set("d16_turned_match", "-")
                checks.append(("turned: the device makes no texture with the rotation SPI", false))
            }
        } catch {
            checks.append(("turned: \(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
