import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// gpu.x1.ce_compressed's multisampled cases: the copy engine reading the sample grid of a stored 2x or 4x BGRA8 target
/// on compressed memory (C32_MS2_2CRA, C32_MS4_2CRA) through a product route, Metal's blit copyFromTexture:toTexture:
/// into a target of its count held on plain memory (switchMetalProbePlainPrivate:). The compressed target is made where
/// the device binds compressed memory (CompressionSettled.remade). A draw whose slanted edge gives a pixel's samples
/// different colours goes into it and into a second plain target, the reference; each sample is read by the texture
/// unit (ms_sample_fragment): `ms<n>_bgra8_rendered` (the compressed target itself, 0 or 1) and `ms<n>_bgra8_wrong`
/// (its copy's samples off the reference's). An M4 Max's blit copy keeps every sample (hwtests12
/// mac/gpu.x1.ce_compressed). A facade without the selector counts `ms_cases_missing` 1, naming why in ms_why.
extension X1CompressedCopyProbe {
    static let plainSelector = NSSelectorFromString("switchMetalProbePlainPrivate:")
    static let msSide = 64

    static func multisampled(_ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String: Int]) async throws {
        guard let facade = gpu.device as? NSObject, facade.responds(to: plainSelector) else {
            metrics.set("ms_why", GM20BFacade.missing("switchMetalProbePlainPrivate:", on: gpu.device))
            wrong["ms_cases_missing"] = 1
            return
        }
        for count in [2, 4] where gpu.device.supportsTextureSampleCount(count) {
            let name = "ms\(count)_bgra8"
            let compressed = try await CompressionSettled.remade(gpu.device) {
                try gpu.texture(.bgra8Unorm, msSide, msSide, samples: count)
            }
            let made = try GM20BProbe.facts(gpu.device, compressed)["comptags"] == 1
            metrics.set("\(name)_made", made ? 1 : 0)
            wrong["\(name)_not_compressed"] = made ? 0 : 1
            guard made else { continue }
            _ = facade.perform(plainSelector, with: NSNumber(value: true))
            let copy: MTLTexture, reference: MTLTexture
            do {
                defer { _ = facade.perform(plainSelector, with: NSNumber(value: false)) }
                copy = try gpu.texture(.bgra8Unorm, msSide, msSide, samples: count)
                reference = try gpu.texture(.bgra8Unorm, msSide, msSide, samples: count)
            }
            let state = try gpu.render(fragment: "comp_pattern_fragment", format: .bgra8Unorm, samples: count)
            for target in [compressed, reference] { try await gpu.run { slanted($0, target, state) } }
            try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(from: compressed, to: copy)
                blit.endEncoding()
            }
            let want = try await sampleGrid(gpu, reference)
            wrong["\(name)_rendered"] = try await sampleGrid(gpu, compressed) == want ? 0 : 1
            let got = try await sampleGrid(gpu, copy)
            wrong["\(name)_wrong"] =
                stride(from: 0, to: min(got.count, want.count), by: 4)
                .filter { got[$0 ..< $0 + 4] != want[$0 ..< $0 + 4] }
                .count + abs(got.count - want.count) / 4
            metrics.set("\(name)_copy_comptags", try GM20BProbe.facts(gpu.device, copy)["comptags"] ?? -1)
        }
    }

    /// A clear to opaque black and the pattern over a left part whose right edge crosses pixels
    static func slanted(_ commands: MTLCommandBuffer, _ target: MTLTexture, _ state: MTLRenderPipelineState) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(state)
        var corners = [
            SIMD4<Float>(-1, -1, 0.5, 1), SIMD4(0.3, -1, 0.5, 1), SIMD4(-1, 1, 0.5, 1), SIMD4(-0.2, 1, 0.5, 1),
        ]
        encoder.setVertexBytes(&corners, length: 64, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
    }

    /// Every sample of a multisampled BGRA8 `texture`, a pixel's samples side by side
    static func sampleGrid(_ gpu: GPU, _ texture: MTLTexture) async throws -> [UInt8] {
        let count = texture.sampleCount
        let grid = try gpu.texture(.bgra8Unorm, texture.width * count, texture.height)
        let state = try gpu.render(fragment: "ms_sample_fragment", format: .bgra8Unorm)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = grid
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: 64, index: 0)
            var samples = UInt32(count)
            encoder.setFragmentBytes(&samples, length: 4, index: 0)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        return try await gpu.read(grid, bytesPerPixel: 4)
    }
}
