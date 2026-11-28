import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import Metal

enum StandInResolveCases {
    struct Drawn {
        let texture: MTLTexture
        let values: [SIMD4<Float>]
        let resolved: [UInt8]
    }

    static let side = 64
    static let counts = [2, 4]
    static let compared = (0 ..< 256).map { 16 * $0 + $0 % 16 }
    static let argb8: [(shift: Int, width: Int)] = [(8, 8), (16, 8), (24, 8), (0, 8)]

    static let formats: [WordMipFormat] = [
        format("f11", 11, bytes: 1, [(0, 8)]),
        format("f41", 41, bytes: 2, [(11, 5), (6, 5), (1, 5), (0, 1)]),
        format("f42", 42, bytes: 2, [(12, 4), (8, 4), (4, 4), (0, 4)]),
        format("f44", 44, bytes: 2, [(8, 4), (4, 4), (0, 4), (12, 4)]),
        format("f93", 93, bytes: 4, [(0, 9), (9, 9), (18, 9)]),
        format("f553", 553, bytes: 8, WordMipFormat.bgra10),
        format("f555", 555, bytes: 4, WordMipFormat.bgr10),
        format("f84", 84, bytes: 4, argb8),
        format("f85", 85, bytes: 4, argb8),
    ]

    static func format(_ key: String, _ raw: UInt, bytes: Int, _ fields: [(shift: Int, width: Int)]) -> WordMipFormat {
        WordMipFormat(key: key, format: MTLPixelFormat(rawValue: raw)!, bytes: bytes, volume: false, fields: fields)
    }

    static func values(_ f: WordMipFormat, samples: Int) -> [SIMD4<Float>] {
        let raw = f.format.rawValue
        let words = P3YcbcrStoreTest.mt19937(
            seed: UInt32(raw * 131) + UInt32(samples), count: side * side * samples * 4
        )
        let floats = words.enumerated().map { i, word -> Float in
            let u = Float(word % 65536) / 65535
            switch raw {
            case 84, 85: return Float(Float16(u))
            case 93: return u * u * 100
            case 553, 555: return i % 4 == 3 ? u : u * 1.5 - 0.25
            default: return u
            }
        }
        return stride(from: 0, to: floats.count, by: 4).map { SIMD4<Float>(floats[$0 ..< $0 + 4]) }
    }

    static func resolved(_ gpu: GPU, _ f: WordMipFormat, samples: Int) async throws -> Drawn {
        let values = values(f, samples: samples)
        let drawn = try gpu.texture(f.format, side, side, usage: [.renderTarget, .shaderRead], samples: samples)
        let one = try gpu.texture(f.format, side, side)
        try await drawSamples(gpu, values, into: drawn, resolve: one, store: .storeAndMultisampleResolve)
        let bytes = try await gpu.read(one, bytesPerPixel: f.bytes)
        return Drawn(
            texture: drawn, values: values, resolved: compared.flatMap { bytes[$0 * f.bytes ..< ($0 + 1) * f.bytes] }
        )
    }

    static func samplesOff(_ gpu: GPU, _ f: WordMipFormat, _ drawn: Drawn) async throws -> SampleRouteReadings {
        let samples = drawn.texture.sampleCount
        let stored = try await sampleReads(gpu, drawn.texture)
        let reader = try gpu.render(fragment: "texel_read", format: .rgba32Float)
        let texels = 0 ..< side * side
        var readings = SampleRouteReadings()
        for sample in 0 ..< samples {
            let alone = try await oneSampleTexture(gpu, f, texels.map { drawn.values[$0 * samples + sample] })
            let fragment = try await YcbcrPackedTargetTest.read(gpu, reader, alone)
            readings.fragmentOff += texels.filter { stored[$0 * samples + sample] != fragment[$0] }.count
            guard readings.computeRefusal == nil else { continue }
            do {
                let compute = try await gpu.texels(alone)
                readings.computeOff += texels.filter { stored[$0 * samples + sample] != compute[$0] }.count
                readings.routesDiffer += texels.filter { compute[$0] != fragment[$0] }.count
                if readings.firstDiffer == "-", let at = texels.first(where: { compute[$0] != fragment[$0] }) {
                    let bytes = try await gpu.read(alone, bytesPerPixel: f.bytes)
                    let code = bytes[at * f.bytes ..< (at + 1) * f.bytes].map { String(format: "%02x", $0) }.joined()
                    let sampleRead = stored[at * samples + sample]
                    readings.firstDiffer =
                        "t\(at) s\(sample) bytes \(code) compute \(compute[at].x),\(compute[at].w) fragment "
                            + "\(fragment[at].x),\(fragment[at].w) stored \(sampleRead.x)"
                }
            } catch {
                readings.computeRefusal = GPU.describe(error)
            }
        }
        return readings
    }

    static func drawSamples(
        _ gpu: GPU, _ values: [SIMD4<Float>]?, into drawn: MTLTexture, resolve: MTLTexture?, store: MTLStoreAction,
        clear: MTLClearColor = MTLClearColor()
    ) async throws {
        let state = try gpu.render(
            fragment: "standin_sample_values", format: drawn.pixelFormat, samples: drawn.sampleCount
        )
        let data = try gpu.buffer(values ?? [])
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = drawn
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = clear
            pass.colorAttachments[0].storeAction = store
            pass.colorAttachments[0].resolveTexture = resolve
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            guard values != nil else { return encoder.endEncoding() }
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            var shape = SIMD2<UInt32>(UInt32(drawn.width), UInt32(drawn.sampleCount))
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBuffer(data, offset: 0, index: 0)
            encoder.setFragmentBytes(&shape, length: 8, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
    }

    static func sampleReads(_ gpu: GPU, _ drawn: MTLTexture) async throws -> [SIMD4<Float>] {
        let (width, height, samples) = (drawn.width, drawn.height, drawn.sampleCount)
        let target = try gpu.texture(.rgba32Float, width * samples, height, usage: [.renderTarget])
        let state = try gpu.render(fragment: "ms_sample_fragment", format: .rgba32Float)
        var count = UInt32(samples)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentTexture(drawn, index: 0)
            encoder.setFragmentBytes(&count, length: 4, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let bytes = try await gpu.read(target, bytesPerPixel: 16)
        return (0 ..< width * height * samples).map { bytes.loaded(as: SIMD4<Float>.self, at: $0) }
    }

    static func oneSampleTexture(_ gpu: GPU, _ f: WordMipFormat, _ values: [SIMD4<Float>]) async throws -> MTLTexture {
        let texture = try gpu.texture(f.format, side, side)
        let state = try gpu.render(fragment: "texel_values", format: f.format)
        let data = try gpu.buffer(values)
        var width = UInt32(side)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBuffer(data, offset: 0, index: 0)
            encoder.setFragmentBytes(&width, length: 4, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        return texture
    }

    static func judged(_ f: WordMipFormat, _ got: [UInt8], apple: [UInt8]) -> (off: Int, far: Int, worst: Int) {
        guard got.count == apple.count else { return (compared.count, compared.count, Int.max) }
        var judged = (off: 0, far: 0, worst: 0)
        for at in stride(from: 0, to: got.count, by: f.bytes) {
            let stored = f.word(got[at ..< at + f.bytes]), wanted = f.word(apple[at ..< at + f.bytes])
            let apart = f.apart(stored, wanted)
            let exact = f.format == .rgb9e5Float ? stored == wanted : f.codes(stored) == f.codes(wanted)
            judged.off += exact ? 0 : 1
            judged.far += apart > 1 ? 1 : 0
            judged.worst = max(judged.worst, apart)
        }
        return judged
    }
}
