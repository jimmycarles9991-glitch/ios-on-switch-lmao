import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Textures bound as an attachment of the other class of format with the same bits (SameBitsCases): a one-sample
/// Depth32Float or Depth16Unorm as colour 0 renders as R32Float or R16Unorm, and an RGBA8Unorm as the depth attachment
/// holds Depth32Float's bits, exact, both through the copy engine on raw. Expected texels are an M4 Max's (bitsprobe.mm,
/// this scene measured): same_bits_d32f_colour 3f400000 left (0.75) / 3e800000 right (the clear's 0.25),
/// same_bits_d16_colour bfff / 4000, same_bits_rgba8_depth 3e800000 (the draw's 0.25) / 3f000000 (the clear's 0.5)
/// with the colour written over the left half alone (_colour_off texels off that), and loaded instead of cleared,
/// 0x11111111 read as a depth fails Less at 0.25: texture and colour unchanged (same_bits_rgba8_depth_load_kept). The
/// pitch texture (over a buffer) is reported, not judged: an M4 Max stores a depth into linear memory in its own order
/// of 8x8 blocks, which the driver does not mirror (colour as depth is a validation error in Metal). _block_linear is
/// the facade's layout of each RGBA8Unorm (switchMetalProbeFacts:, "-" where it does not answer).
struct SameBitsTest: HWTest {
    let id = "gpu.gm20b.same_bits"
    let name = "A depth texture as colour and a colour texture as depth hold each other's bits, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let depthAsColour:
        [(
            key: String, format: MTLPixelFormat, state: MTLPixelFormat, bytes: Int, left: String,
            right: String
        )] = [
            ("same_bits_d32f_colour", .depth32Float, .r32Float, 4, "3f400000", "3e800000"),
            ("same_bits_d16_colour", .depth16Unorm, .r16Unorm, 2, "bfff", "4000"),
        ]

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
        for c in depthAsColour {
            do {
                let bytes = try await SameBitsCases.depthAsColour(gpu, c.format, state: c.state, bytes: c.bytes)
                let (left, right) = SameBitsCases.halves(bytes, size: c.bytes)
                metrics.set(c.key + "_left", left)
                metrics.set(c.key + "_right", right)
                checks.append(("\(c.key): \(c.left) left, \(c.right) right", left == c.left && right == c.right))
            } catch {
                checks.append(("\(c.key): \(GPU.describe(error))", false))
            }
        }
        for (key, pitch) in [("same_bits_rgba8_depth", false), ("same_bits_rgba8_depth_pitch", true)] {
            do {
                let r = try await SameBitsCases.colourAsDepth(gpu, pitch: pitch, load: false)
                let (left, right) = SameBitsCases.halves(r.depth, size: 4)
                let off = SameBitsCases.colourOff(r.colour, written: true)
                metrics.set(key + "_left", left)
                metrics.set(key + "_right", right)
                metrics.set(key + "_colour_off", off)
                metrics.set(key + "_block_linear", r.blockLinear.map(String.init) ?? "-")
                checks.append(("\(key): colour written over the left half", off == 0))
                guard !pitch else { continue }
                checks.append(("\(key): 3e800000 left, 3f000000 right", left == "3e800000" && right == "3f000000"))
                if let blockLinear = r.blockLinear {
                    checks.append(
                        ("\(key): \(pitch ? "pitch" : "block-linear") memory", blockLinear == (pitch ? 0 : 1))
                    )
                }
            } catch {
                checks.append(("\(key): \(GPU.describe(error))", false))
            }
        }
        do {
            let r = try await SameBitsCases.colourAsDepth(gpu, pitch: false, load: true)
            let kept = r.depth.allSatisfy { $0 == 0x11 } && SameBitsCases.colourOff(r.colour, written: false) == 0
            metrics.set("same_bits_rgba8_depth_load_kept", kept)
            checks.append(("loaded 0x11 bytes fail the test: texture and colour unchanged", kept))
        } catch {
            checks.append(("same_bits_rgba8_depth_load: \(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
