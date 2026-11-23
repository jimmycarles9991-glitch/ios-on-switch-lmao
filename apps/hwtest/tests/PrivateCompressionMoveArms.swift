import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.gm20b.private_compression's arms array1 and ms4_move (drivers/TegraGM20BMetal/private_comp.h: a
/// one-level array or a 2x/4x target made plain for want of compressed memory moves onto it, every layer and sample,
/// copyImages): a private BGRA8 2D array of 3 layers of 1024x1024 and a private 4x BGRA8 512x512 target, each larger
/// than any of its kind an earlier test makes, so made plain (`array_at_creation`, `ms_at_creation`; kept memory of a
/// kind at least as large would serve one at creation: CompQueue::fit); the compression thread settled
/// (CompressionSettled); then one command buffer clears each layer to a colour of its own and draws another over its
/// left half, and draws the 4x target's slanted pattern (a pixel on the edge has samples of both); both move at that
/// command buffer's submission, before its encode. Judged: both on compressed memory after (`array_comptags_after`,
/// `ms_comptags_after` 1), every layer's texels its codes (`layer_wrong` 0), every sample the same as the same pass
/// into a 4x target held plain (switchMetalProbePlainPrivate:, `sample_wrong` 0), and `moves` (private_compressed_moves)
/// as many as were made plain. Red: a layer past the first lost in the move (the want's bytes short of storageRange, or
/// copyImages missed a layer), or the sample grid's copy wrong.
extension PrivateCompressionTest {
    static let moveLayers = 3, moveArraySide = 1024, moveSamplesSide = 512

    /// Layer k's clear and drawn colours as BGRA8 bytes
    static func layerBytes(_ k: Int, drawn: Bool) -> [UInt8] {
        drawn
            ? [UInt8(250 - 50 * k), UInt8(30 + 70 * k), 120, 255]
            : [UInt8(10 + 20 * k), UInt8(200 - 30 * k), UInt8(60 + 40 * k), 255]
    }

    static func floats(_ b: [UInt8]) -> SIMD4<Float> {
        SIMD4(Float(b[2]) / 255, Float(b[1]) / 255, Float(b[0]) / 255, Float(b[3]) / 255)
    }

    func moveArms(_ gpu: GPU, _ facade: NSObject, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        let movesBefore = GM20BFacade.counters(of: gpu.device)["private_compressed_moves"] ?? 0
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: Self.moveArraySide, height: Self.moveArraySide, mipmapped: false
        )
        d.textureType = .type2DArray
        d.arrayLength = Self.moveLayers
        d.storageMode = .private
        d.usage = [.renderTarget, .shaderRead]
        guard let array = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no 2D array made") }
        let samples = try gpu.texture(.bgra8Unorm, Self.moveSamplesSide, Self.moveSamplesSide, samples: 4)
        let arrayAt = comptags(facade, array), samplesAt = comptags(facade, samples)
        try await CompressionSettled.settle(gpu.device)
        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let pattern = try gpu.render(fragment: "comp_pattern_fragment", format: .bgra8Unorm, samples: 4)
        let side = Self.moveArraySide
        var run = ShapeRun()
        run.paths.append(
            try await gpu.run { commands in
                for layer in 0 ..< Self.moveLayers {
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = array
                    pass.colorAttachments[0].slice = layer
                    pass.colorAttachments[0].loadAction = .clear
                    let clear = Self.floats(Self.layerBytes(layer, drawn: false))
                    pass.colorAttachments[0].clearColor = MTLClearColor(
                        red: Double(clear.x), green: Double(clear.y), blue: Double(clear.z), alpha: 1
                    )
                    pass.colorAttachments[0].storeAction = .store
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(solid)
                    GPU.draw(
                        encoder, corners: GPU.pixelQuad(0, 0, side / 2, side, width: side, height: side),
                        color: Self.floats(Self.layerBytes(layer, drawn: true))
                    )
                    encoder.endEncoding()
                }
                Self.slanted(commands, samples, pattern)
            }.path
        )
        let arrayAfter = comptags(facade, array), samplesAfter = comptags(facade, samples)
        let moves = (GM20BFacade.counters(of: gpu.device)["private_compressed_moves"] ?? 0) - movesBefore
        var layerWrong = 0
        for layer in 0 ..< Self.moveLayers {
            let bytes = try await gpu.read(array, bytesPerPixel: 4, slice: layer)
            let clear = Self.layerBytes(layer, drawn: false), drawn = Self.layerBytes(layer, drawn: true)
            for y in 0 ..< side {
                for x in 0 ..< side where !bytes.pixel(x, y, width: side).elementsEqual(x < side / 2 ? drawn : clear) {
                    layerWrong += 1
                }
            }
        }
        let got = try await sampleGrid(gpu, samples, &run)
        _ = facade.perform(Self.plainSelector, with: NSNumber(value: true))
        let reference: MTLTexture
        do {
            defer { _ = facade.perform(Self.plainSelector, with: NSNumber(value: false)) }
            reference = try gpu.texture(.bgra8Unorm, Self.moveSamplesSide, Self.moveSamplesSide, samples: 4)
        }
        run.paths.append(try await gpu.run { Self.slanted($0, reference, pattern) }.path)
        let want = try await sampleGrid(gpu, reference, &run)
        let sampleWrong =
            stride(from: 0, to: min(got.count, want.count), by: 4).filter { got[$0 ..< $0 + 4] != want[$0 ..< $0 + 4] }
                .count + abs(got.count - want.count) / 4
        let wantMoves = (arrayAt == 0 ? 1 : 0) + (samplesAt == 0 ? 1 : 0)
        metrics.set("array_at_creation", arrayAt)
        metrics.set("ms_at_creation", samplesAt)
        metrics.set("array_comptags_after", arrayAfter)
        metrics.set("ms_comptags_after", samplesAfter)
        metrics.set("moves", moves)
        metrics.set("layer_wrong", layerWrong)
        metrics.set("sample_wrong", sampleWrong)
        if arrayAfter != 1 { wrong.append("array1_not_compressed") }
        if samplesAfter != 1 { wrong.append("ms4_move_not_compressed") }
        if moves != wantMoves { wrong.append("moves") }
        if layerWrong > 0 { wrong.append("layer_wrong") }
        if sampleWrong > 0 { wrong.append("sample_wrong") }
        if run.paths.contains(where: { $0 != "gm20b" }) { wrong.append("move_arms_path") }
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
        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
    }
}
