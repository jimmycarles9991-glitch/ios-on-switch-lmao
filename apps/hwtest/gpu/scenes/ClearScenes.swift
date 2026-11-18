import Foundation
import HWTestCore
import Metal

/// Load-action clears of every common format into private (block-linear where the shape tiles) targets, small and
/// large, read back by the blit engine. A pass with nothing but a clear needs no pipeline state, so on the raw facade
/// it is the raw path's own: its fast clears and the kernel's zero-bandwidth-clear table (ZBC, method 19), whose
/// few entries the many distinct colours here overflow, so both kinds of clear run.
struct ClearFormatsScene: GPUScene {
    let id = "gpu.clear.formats"
    let name = "Fast and ZBC clears of every common format"

    static let colors: [[Float]] = [
        [0, 0, 0, 0], [1, 1, 1, 1], [0, 0, 0, 1], [0.2, 0.4, 0.6, 1], [1, 0, 0, 1], [0.5, 0.25, 0.75, 0.5],
    ]
    static let depths: [Float] = [0, 1, 0.5, 0.25, 0.123]
    static let stencils: [Float] = [0, 255, 0x5A]
    static let sizes = [(13, 7), (256, 256), (1280, 720)]
    static let formats: [TexelFormat] = [
        .rgba8Unorm, .bgra8Unorm, .rgba8Srgb, .bgra8Srgb, .r8Unorm, .rg8Unorm, .rgba16Float, .r16Float, .rg16Float,
        .rgba32Float, .r32Float, .rgb10a2Unorm, .bgr10XR, .bgra10XR, .depth32Float, .depth16Unorm, .stencil8,
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        var skipped: [String] = []
        for format in Self.formats {
            guard format.supported(on: gpu.device, usage: .renderTarget) else {
                skipped.append(format.rawValue)
                continue
            }
            for (size, (width, height)) in Self.sizes.enumerated() {
                let target = try gpu.texture(format.metal, width, height, usage: [.renderTarget, .shaderRead])
                let values = Self.values(format).prefix(size == 2 ? 2 : 6)
                for value in values {
                    try await gpu.run { commands in
                        commands.makeRenderCommandEncoder(descriptor: Self.clearPass(target, format, value))!
                            .endEncoding()
                    }
                    let got = try await gpu.read(target, bytesPerPixel: format.bytesPerTexel)
                    let texel = format.encode(Self.stored(format, value))
                    result.compare(
                        "\(format.rawValue) \(width)x\(height) clear \(value.map { "\($0)" }.joined(separator: ","))",
                        bytes: got, expected: Patterns.repeated(texel, count: width * height),
                        format: format, width: width, height: height,
                        tolerance: format.step(near: value.max() ?? 1) * 1.01)
                }
            }
        }
        if !skipped.isEmpty { result.notes.append("not renderable here: " + skipped.joined(separator: ",")) }
        return result
    }

    static func values(_ format: TexelFormat) -> [[Float]] {
        switch format {
        case .depth32Float, .depth16Unorm: return depths.map { [$0] }
        case .stencil8: return stencils.map { [$0] }
        default: return colors
        }
    }

    /// The clear colour's channels the format keeps, in raster order (A8 none but alpha, R8 red, ...).
    static func stored(_ format: TexelFormat, _ value: [Float]) -> [Float] {
        if format.isDepthOrStencil { return value }
        switch format.channels {
        case 1: return [value[0]]
        case 2: return [value[0], value[1]]
        case 3: return [value[0], value[1], value[2]]
        default: return value
        }
    }

    static func clearPass(_ target: MTLTexture, _ format: TexelFormat, _ value: [Float]) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        switch format {
        case .depth32Float, .depth16Unorm:
            pass.depthAttachment.texture = target
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = Double(value[0])
            pass.depthAttachment.storeAction = .store
        case .stencil8:
            pass.stencilAttachment.texture = target
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.clearStencil = UInt32(value[0])
            pass.stencilAttachment.storeAction = .store
        default:
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(
                red: Double(value[0]), green: Double(value[1]), blue: Double(value[2]), alpha: Double(value[3]))
            pass.colorAttachments[0].storeAction = .store
        }
        return pass
    }
}

/// A large target cleared (raw on the facade, compressible and ZBC-cleared where the kernel compresses), drawn over
/// in part by a pipeline of the app's own (m2v on the facade, through its alias of the raw path's memory), cleared
/// again and loaded and drawn again: each step read back whole. A draw that sees the cleared memory in another
/// compression state or layout than the clear left it shows up as blocks or streaks here.
struct ClearThenDrawScene: GPUScene {
    let id = "gpu.clear.then_draw"
    let name = "Clear, draw over part, clear again, load and draw"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let (width, height) = (1280, 720)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        for format in [TexelFormat.rgba8Unorm] {
            let target = try gpu.texture(format.metal, width, height)
            var model = Raster(width: width, height: height, channels: 4) { _, _, _ in 0 }
            let steps: [(clear: [Float]?, rect: [Int], color: [Float])] = [
                ([0.2, 0.4, 0.6, 1], [100, 37, 517, 301], [1, 0, 0, 1]),
                ([0, 0, 0, 0], [], []),
                (nil, [640, 360, 1280, 720], [0, 1, 0, 1]),
                ([1, 1, 1, 1], [3, 5, 10, 711], [0, 0, 1, 1]),
            ]
            for (index, step) in steps.enumerated() {
                try await gpu.run { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target
                    pass.colorAttachments[0].storeAction = .store
                    if let clear = step.clear {
                        pass.colorAttachments[0].loadAction = .clear
                        pass.colorAttachments[0].clearColor = MTLClearColor(
                            red: Double(clear[0]), green: Double(clear[1]), blue: Double(clear[2]),
                            alpha: Double(clear[3]))
                    } else {
                        pass.colorAttachments[0].loadAction = .load
                    }
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    if !step.rect.isEmpty {
                        encoder.setRenderPipelineState(pipeline)
                        let r = step.rect
                        GPU.draw(
                            encoder, corners: GPU.pixelQuad(r[0], r[1], r[2], r[3], width: width, height: height),
                            color: SIMD4(step.color[0], step.color[1], step.color[2], step.color[3]))
                    }
                    encoder.endEncoding()
                }
                if let clear = step.clear {
                    model = Raster(width: width, height: height, channels: 4) { _, _, c in clear[c] }
                }
                if !step.rect.isEmpty {
                    let r = step.rect
                    for y in r[1]..<r[3] {
                        for x in r[0]..<r[2] { for c in 0..<4 { model[x, y, c] = step.color[c] } }
                    }
                }
                let got = try await gpu.raster(target, format)
                result.compare("\(format.rawValue) step \(index)", got, format.quantized(model), tolerance: 1.01 / 255)
            }
        }
        return result
    }
}
