import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.x1.ce_compressed's multisampled cases: the copy engine copying the sample grid of a stored 2x or 4x BGRA8 target
/// on compressed memory (C32_MS2_2CRA, C32_MS4_2CRA) into plain memory, as the target moves before an m2v twin
/// (raw_private_comp.cpp copyImages; forced here by switchRawProbeMovePlain:). Each sample is read by the texture unit
/// (ms_sample_fragment) after a draw whose slanted edge gives a pixel's samples different colours: on the compressed
/// target, then again once it moved, each against a target held on plain memory (switchRawProbePlainPrivate:).
extension X1CompressedCopyProbe {
    static let plainSelector = NSSelectorFromString("switchRawProbePlainPrivate:")
    static let moveSelector = NSSelectorFromString("switchRawProbeMovePlain:")

    static func multisampled(_ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String: Int]) async throws {
        guard let facade = gpu.device as? NSObject, facade.responds(to: moveSelector) else { return }
        for count in [2, 4] where gpu.device.supportsTextureSampleCount(count) {
            let name = "ms\(count)_bgra8"
            var compressed = try gpu.texture(.bgra8Unorm, 64, 64, samples: count)
            var tries = 0
            while try RawProbe.facts(gpu.device, compressed)["comptags"] != 1 && tries < 20 {
                try await Task.sleep(nanoseconds: 50_000_000)
                compressed = try gpu.texture(.bgra8Unorm, 64, 64, samples: count)
                tries += 1
            }
            let made = try RawProbe.facts(gpu.device, compressed)["comptags"] == 1
            metrics.set("\(name)_made", made ? 1 : 0)
            wrong["\(name)_not_compressed"] = made ? 0 : 1
            guard made else { continue }
            _ = facade.perform(plainSelector, with: NSNumber(value: true))
            let plain: MTLTexture
            do {
                defer { _ = facade.perform(plainSelector, with: NSNumber(value: false)) }
                plain = try gpu.texture(.bgra8Unorm, 64, 64, samples: count)
            }
            let state = try gpu.render(fragment: "comp_pattern_fragment", format: .bgra8Unorm, samples: count)
            for target in [compressed, plain] { try await gpu.run { slanted($0, target, state) } }
            let reference = try await sampleGrid(gpu, plain)
            wrong["\(name)_rendered"] = try await sampleGrid(gpu, compressed) == reference ? 0 : 1
            let moved = (facade.perform(moveSelector, with: compressed)?.takeUnretainedValue() as? NSNumber)?.boolValue
            metrics.set("\(name)_moved", moved == true ? 1 : 0)
            metrics.set("\(name)_after_move_comptags", try RawProbe.facts(gpu.device, compressed)["comptags"] ?? -1)
            wrong["\(name)_not_moved"] = moved == true ? 0 : 1
            wrong["\(name)_moved_ce"] = try await sampleGrid(gpu, compressed) == reference ? 0 : 1
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
