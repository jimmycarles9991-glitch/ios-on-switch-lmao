import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A BGRA10_XR_sRGB (553) pass that draws nothing keeps every code, alpha past 894 included: an 8x8 shared texture
/// whose texels are red 0, green 200, blue 1000, alpha 1000 (replaceRegion), a pass of Load and Store with no draw and
/// one of DontCare and Store with no draw, each read back by getBytes. An M4 Max keeps every word in both (m2v-textures4
/// probes/xr553.mm, and this test's case code on it), so alpha_load= and alpha_dontcare= are 1000 and off= (texels whose
/// word changed) 0. Reported, not judged: over a 64x64 texture of the same texels, a draw of pixels 4 to 11 of each axis
/// inside the 32x32 tile at the origin; the M4 Max clamps alpha to 894 in that tile alone (probes/more.mm corner, and
/// this case code), so alpha_touched_tile= (texel 20,20, undrawn) 894 and alpha_untouched_tile= (texel 52,52) 1000
/// (touched_tile_error= in their place if that arm throws). Every command buffer raw, failed_cbs=0.
struct Bgra10XrSrgbEmptyPassTest: HWTest {
    let id = "gpu.gm20b.bgra10xrsrgb_empty_pass"
    let name = "A BGRA10_XR_sRGB pass that draws nothing keeps alpha codes past 894, as an M4 Max does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    static let written = [0, 200, 1000, 1000]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var off = 0
        for (key, load) in [("alpha_load", MTLLoadAction.load), ("alpha_dontcare", .dontCare)] {
            do {
                let texels = try await Self.drawnNothing(gpu, load: load)
                let changed = texels.filter { $0 != Self.written }.count
                metrics.set(key, texels[0][3])
                off += changed
                checks.append(("\(key): every word kept", changed == 0))
            } catch {
                checks.append(("\(key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("off", off)
        do {
            let tiles = try await Self.touchedTile(gpu)
            metrics.set("alpha_touched_tile", tiles.touched)
            metrics.set("alpha_untouched_tile", tiles.untouched)
        } catch {
            metrics.set("touched_tile_error", GPU.describe(error))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: ["alpha_load", "alpha_dontcare", "failed_cbs"])
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func filled(
        _ gpu: GPU, side: Int, usage: MTLTextureUsage = [.renderTarget, .shaderRead]
    ) throws
        -> MTLTexture
    {
        let texture = try gpu.texture(.bgra10_xr_srgb, side, side, usage: usage, storage: .shared)
        let bytes = [UInt8]((0 ..< (side * side)).map { _ in XrSrgbFormat.bgra.texel(written) }.joined())
        texture.replace(
            region: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 0, withBytes: bytes, bytesPerRow: side * 8
        )
        return texture
    }

    static func codes(_ texture: MTLTexture) -> [[Int]] {
        var bytes = [UInt8](repeating: 0, count: texture.width * texture.height * 8)
        texture.getBytes(
            &bytes, bytesPerRow: texture.width * 8, from: MTLRegionMake2D(0, 0, texture.width, texture.height),
            mipmapLevel: 0
        )
        return XrSrgbFormat.bgra.codes(bytes)
    }

    static func drawnNothing(_ gpu: GPU, load: MTLLoadAction, side: Int = 8) async throws -> [[Int]] {
        let texture = try filled(gpu, side: side)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = load
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        return codes(texture)
    }

    static func touchedTile(
        _ gpu: GPU, usage: MTLTextureUsage = [.renderTarget, .shaderRead]
    ) async throws -> (
        touched: Int, untouched: Int
    ) {
        let side = 64
        let texture = try filled(gpu, side: side, usage: usage)
        let state = try gpu.render(fragment: "solid_fragment", format: .bgra10_xr_srgb)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            GPU.draw(
                encoder, corners: GPU.pixelQuad(4, 4, 12, 12, width: side, height: side),
                color: SIMD4(0.5, 0.25, 0, 0.5)
            )
            encoder.endEncoding()
        }
        let texels = codes(texture)
        return (texels[20 * side + 20][3], texels[52 * side + 52][3])
    }
}
