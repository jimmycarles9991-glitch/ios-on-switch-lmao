import Foundation
import HWTestCore
import Metal

struct BlendCase {
    let state: BlendState
    var writeMask: MTLColorWriteMask = .all
}

/// Blend states drawn one 4x4 cell each over a known destination and checked against Metal's blend equation worked
/// on the CPU (core/Blend.swift).
struct BlendGrid {
    static let cell = 4
    static let columns = 16

    let format: TexelFormat
    let source: [Float]
    let source1: [Float]
    let destination: [Float]
    let constant: [Float]
    let normalized: Bool

    func run(_ gpu: GPU, _ cases: [BlendCase], _ result: inout SceneResult) async throws {
        let rows = (cases.count + Self.columns - 1) / Self.columns
        let (width, height) = (Self.columns * Self.cell, rows * Self.cell)
        let target = try gpu.texture(format.metal, width, height, usage: [.renderTarget, .shaderRead])
        let stored = Array(destination.prefix(format.channels))
        let fill = Patterns.repeated(format.encode(stored), count: width * height)
        try await gpu.blitUpload(
            [UploadRect(x: 0, y: 0, width: width, height: height)], [fill], into: target, texel: format.bytesPerTexel)
        var pipelines: [MTLRenderPipelineState?] = []
        for blend in cases {
            do {
                pipelines.append(try pipeline(gpu, blend))
            } catch {
                result.notes.append("\(blend.state.name): no pipeline (\(GPU.describe(error)))")
                pipelines.append(nil)
            }
        }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setBlendColor(red: constant[0], green: constant[1], blue: constant[2], alpha: constant[3])
            for (index, pipeline) in pipelines.enumerated() {
                guard let pipeline else { continue }
                let (x, y) = (index % Self.columns * Self.cell, index / Self.columns * Self.cell)
                encoder.setRenderPipelineState(pipeline)
                var colors = [
                    SIMD4(source[0], source[1], source[2], source[3]),
                    SIMD4(source1[0], source1[1], source1[2], source1[3]),
                ]
                var corners = GPU.pixelQuad(x, y, x + Self.cell, y + Self.cell, width: width, height: height)
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentBytes(&colors, length: MemoryLayout<SIMD4<Float>>.stride * 2, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
        let got = try await gpu.raster(target, format)
        for (index, blend) in cases.enumerated() where pipelines[index] != nil {
            let (x, y) = (index % Self.columns * Self.cell, index / Self.columns * Self.cell)
            var expected = Blend.apply(
                blend.state, source: source, source1: source1, destination: destinationAsRead, constant: constant,
                normalized: normalized)
            for (c, bit) in [MTLColorWriteMask.red, .green, .blue, .alpha].enumerated()
            where !blend.writeMask.contains(bit) {
                expected[c] = destinationAsRead[c]
            }
            let texel = format.decode(format.encode(Array(expected.prefix(format.channels)))[...])
            let want = Raster(width: Self.cell, height: Self.cell, channels: format.channels, levels: format.levels) {
                _, _, c in texel[c]
            }
            let mask = blend.writeMask == .all ? "" : " mask \(blend.writeMask.rawValue)"
            result.compare(
                "\(format.rawValue) \(blend.state.name)\(mask)",
                got.cropped(x: x, y: y, width: Self.cell, height: Self.cell),
                want, tolerance: texel.map { format.step(near: $0) }.max()! * 1.51)
        }
    }

    /// The destination as the blender reads it: what the format stored, alpha 1 where the format has none.
    var destinationAsRead: [Float] {
        let stored = format.decode(format.encode(Array(destination.prefix(format.channels)))[...])
        return (0..<4).map { $0 < stored.count ? stored[$0] : 1 }
    }

    func pipeline(_ gpu: GPU, _ blend: BlendCase) throws -> MTLRenderPipelineState {
        try gpu.render(fragment: blend.state.dualSource ? "dual_fragment" : "solid_fragment", format: format.metal) {
            let attachment = $0.colorAttachments[0]!
            attachment.isBlendingEnabled = true
            attachment.writeMask = blend.writeMask
            attachment.rgbBlendOperation = MTLBlendOperation(rawValue: UInt(blend.state.rgbOperation.rawValue))!
            attachment.alphaBlendOperation = MTLBlendOperation(rawValue: UInt(blend.state.alphaOperation.rawValue))!
            attachment.sourceRGBBlendFactor = MTLBlendFactor(rawValue: UInt(blend.state.sourceRGB.rawValue))!
            attachment.destinationRGBBlendFactor = MTLBlendFactor(rawValue: UInt(blend.state.destinationRGB.rawValue))!
            attachment.sourceAlphaBlendFactor = MTLBlendFactor(rawValue: UInt(blend.state.sourceAlpha.rawValue))!
            attachment.destinationAlphaBlendFactor = MTLBlendFactor(
                rawValue: UInt(blend.state.destinationAlpha.rawValue))!
        }
    }
}

/// Every blend factor as source and as destination, every operation, separate alpha and a write mask, on RGBA8.
struct UnormBlendScene: GPUScene {
    let id = "gpu.blend.unorm8"
    let name = "Every blend factor and operation on RGBA8"

    static var cases: [BlendCase] {
        var cases = BlendFactor.allCases.flatMap { factor in
            [
                BlendCase(state: BlendState(source: factor, destination: .zero)),
                BlendCase(state: BlendState(source: .zero, destination: factor)),
            ]
        }
        for operation in BlendOperation.allCases {
            cases.append(BlendCase(state: BlendState(operation, source: .one, destination: .one)))
            cases.append(
                BlendCase(state: BlendState(operation, source: .sourceAlpha, destination: .oneMinusSourceAlpha)))
        }
        cases += [
            BlendCase(
                state: BlendState(
                    source: .sourceAlpha, destination: .oneMinusSourceAlpha, sourceAlpha: .one,
                    destinationAlpha: .oneMinusSourceAlpha)),
            BlendCase(state: BlendState(.subtract, source: .one, destination: .one, alphaOperation: .reverseSubtract)),
            BlendCase(
                state: BlendState(
                    source: .destinationColor, destination: .sourceColor, sourceAlpha: .blendAlpha,
                    destinationAlpha: .oneMinusBlendColor)),
            BlendCase(state: BlendState(source: .one, destination: .one), writeMask: [.red, .alpha]),
        ]
        return cases
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let grid = BlendGrid(
            format: .rgba8Unorm, source: [0.8, 0.4, 0.2, 0.6], source1: [0.2, 0.6, 1, 0.4],
            destination: [0.4, 0.6, 0.8, 0.2], constant: [0.6, 0.2, 0.4, 0.8], normalized: true)
        try await grid.run(gpu, Self.cases, &result)
        return result
    }
}

/// Blending past [0, 1] on the extended-range formats Core Animation composites wide colour in (BGR10_XR,
/// BGRA10_XR) and on RGBA16Float: nothing is clamped to [0, 1], results are clamped to what the format holds.
struct ExtendedBlendScene: GPUScene {
    let id = "gpu.blend.extended"
    let name = "Blending on extended-range formats"

    static let cases: [BlendCase] = [
        BlendState(source: .one, destination: .one),
        BlendState(source: .sourceAlpha, destination: .oneMinusSourceAlpha),
        BlendState(.subtract, source: .one, destination: .one),
        BlendState(.reverseSubtract, source: .one, destination: .one),
        BlendState(.min, source: .one, destination: .one), BlendState(.max, source: .one, destination: .one),
        BlendState(source: .destinationColor, destination: .zero),
        BlendState(source: .one, destination: .oneMinusSourceColor),
        BlendState(source: .blendColor, destination: .oneMinusBlendAlpha),
    ].map { BlendCase(state: $0) }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        for format in [TexelFormat.rgba16Float, .bgr10XR, .bgra10XR] {
            guard format.supported(on: gpu.device, usage: .renderTarget) else {
                result.notes.append("\(format.rawValue) not renderable here")
                continue
            }
            let grid = BlendGrid(
                format: format, source: [1.2, -0.25, 0.6, 0.5], source1: [0, 0, 0, 0],
                destination: [0.4, 1.1, -0.5, 0.8],
                constant: [0.5, 0.25, 1, 0.75], normalized: false)
            try await grid.run(gpu, Self.cases, &result)
        }
        return result
    }
}
