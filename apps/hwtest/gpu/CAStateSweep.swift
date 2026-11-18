import Foundation
import HWTestCore
import Metal
import simd

/// One Core Animation state drawn the same way on any device, for comparing the raw facade's pixels with the m2v
/// device's (tests/CAStateSweepTest.swift). The state is made from QuartzCore's library exactly as its corpus line
/// records it; every attachment is cleared, then two draws (a slanted strip and a small rectangle, so blending,
/// clears and partial tiles all show) read CA's bindings: vertices at vertex buffer 1, an identity transform at 2,
/// patterned payloads at every other buffer slot CA uses, a patterned texture at every fragment texture slot and a
/// linear sampler at every sampler slot. Each attachment's bytes are read back with the blit engine.
public enum CAStateSweep {
    public static let width = 72
    public static let height = 40

    public struct Drawn: Sendable {
        public let path: String
        public let attachments: [[UInt8]]
    }

    public struct Unsupported: Error, CustomStringConvertible {
        public let description: String
    }

    /// Bytes per texel of the color formats Core Animation's states render to (Metal's pixel format numbers).
    public static func bytesPerTexel(_ format: UInt) -> Int? {
        switch format {
        case 1, 10, 11: return 1
        case 20, 25, 30: return 2
        case 55, 60, 65, 70, 71, 80, 81, 90, 92, 94, 554, 555: return 4
        case 105, 110, 115, 550, 552, 553: return 8
        case 125: return 16
        default: return nil
        }
    }

    /// The corpus's Core Animation lines (ca_states.log in the app's bundle), one per distinct class when `everyState`
    /// is false: attachment formats, the first attachment's blending and the two functions.
    public static func states(_ text: String, everyState: Bool) -> [PipelineLine] {
        var seen = Set<String>()
        return text.split(separator: "\n").compactMap { PipelineLine(String($0)) }.filter { line in
            everyState || seen.insert(classKey(line)).inserted
        }
    }

    /// A state's identity in the reference (core/CAGoldens.swift): every field of its corpus line, hashed.
    public static func key(_ line: PipelineLine) -> String {
        let function = { (f: PipelineLine.Function) in
            f.name
                + f.constants.map { c in "\(c.name):\(c.type):" + c.bytes.map { String(format: "%02x", $0) }.joined() }
                .joined(separator: ",")
        }
        let fields =
            [line.label, "\(line.samples)", "\(line.topology)", "\(line.depthFormat)/\(line.stencilFormat)"]
            + line.colors.map { "c\($0.index)=\($0.fields)" } + line.attributes.map { "a\($0.index)=\($0.fields)" }
            + [function(line.vertex), function(line.fragment)]
        return String(Signature.fnv1a(Array(fields.joined(separator: " ").utf8)), radix: 16)
    }

    static func classKey(_ line: PipelineLine) -> String {
        let colors = line.colors.map { $0.fields.map(String.init).joined(separator: ":") }.joined(separator: ",")
        return "\(colors) \(line.depthFormat)/\(line.stencilFormat) \(line.vertex.name) \(line.fragment.name)"
    }

    /// `premultiplied`: every colour premultiplied (vertices, pattern, clear), the only colours Core Animation blends
    public static func draw(
        _ line: PipelineLine, library: MTLLibrary, gpu: GPU, premultiplied: Bool = false
    ) async throws -> Drawn {
        guard line.samples == 1, line.depthFormat == 0, line.stencilFormat == 0 else {
            throw Unsupported(description: "samples \(line.samples), depth/stencil \(line.depthFormat)")
        }
        guard line.attributes.allSatisfy({ $0.fields[3] == UInt(MemoryLayout<CAState.Vertex>.stride) }) else {
            throw Unsupported(description: "a vertex layout other than CA's 48-byte vertex")
        }
        let pipeline: MTLRenderPipelineState
        do {
            pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
        } catch {
            throw Unsupported(description: "not this QuartzCore's state (\(GPU.describe(error)))")
        }
        var targets: [MTLTexture] = []
        for color in line.colors.sorted(by: { $0.index < $1.index }) {
            guard bytesPerTexel(color.format) != nil, let format = MTLPixelFormat(rawValue: color.format) else {
                throw Unsupported(description: "attachment format \(color.format)")
            }
            targets.append(try gpu.texture(format, width, height))
        }
        let source = try patternTexture(gpu, premultiplied: premultiplied)
        let sampler = try linearSampler(gpu.device)
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            for (index, target) in targets.enumerated() {
                pass.colorAttachments[index].texture = target
                pass.colorAttachments[index].loadAction = .clear
                pass.colorAttachments[index].clearColor = premultiplied ? premultipliedClear : clear
                pass.colorAttachments[index].storeAction = .store
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            bind(encoder, source: source, sampler: sampler)
            for vertices in [strip(), rectangle()] {
                var vertices = premultiplied ? vertices.map(premultiply) : vertices
                encoder.setVertexBytes(
                    &vertices, length: MemoryLayout<CAState.Vertex>.stride * vertices.count, index: 1)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: vertices.count)
            }
            encoder.endEncoding()
        }
        if timing.path.hasPrefix("error:") { throw TestFailure("draw: \(timing.path)") }
        var attachments: [[UInt8]] = []
        for (target, color) in zip(targets, line.colors.sorted(by: { $0.index < $1.index })) {
            attachments.append(try await readBack(target, bytesPerPixel: bytesPerTexel(color.format)!, gpu: gpu))
        }
        return Drawn(path: timing.path, attachments: attachments)
    }

    /// What the readback buffer holds before the blit: bytes a blit must leave alone (550's pad) still read as it.
    public static let sentinel: UInt8 = 0xA5

    static func readBack(_ texture: MTLTexture, bytesPerPixel: Int, gpu: GPU) async throws -> [UInt8] {
        let rowBytes = texture.width * bytesPerPixel
        let out = try gpu.buffer([UInt8](repeating: sentinel, count: rowBytes * texture.height))
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: texture.width, height: texture.height, depth: 1), to: out,
                destinationOffset: 0, destinationBytesPerRow: rowBytes, destinationBytesPerImage: 0)
            blit.endEncoding()
        }
        return [UInt8](UnsafeRawBufferPointer(start: out.contents(), count: rowBytes * texture.height))
    }

    static let clear = MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 0.625)
    static let premultipliedClear = MTLClearColor(red: 0.125, green: 0.25, blue: 0.375, alpha: 0.625)

    static func premultiply(_ v: CAState.Vertex) -> CAState.Vertex {
        var out = v
        out.color = SIMD4(v.color.x * v.color.w, v.color.y * v.color.w, v.color.z * v.color.w, v.color.w)
        return out
    }

    static func rectangle() -> [CAState.Vertex] {
        CAState.rectangle(x0: 0.4, y0: -0.9, x1: 0.85, y1: -0.3, color: SIMD4(0.9, 0.2, 0.6, 0.5))
    }

    /// A quad slanted across the target, its colours and texture coordinates varying per corner.
    static func strip() -> [CAState.Vertex] {
        [
            CAState.Vertex(-0.95, -0.6, u: 0, v: 1, color: SIMD4(1, 0.5, 0, 0.75)),
            CAState.Vertex(0.3, -0.95, u: 1, v: 1, color: SIMD4(0, 1, 0.25, 1)),
            CAState.Vertex(-0.6, 0.9, u: 0, v: 0, color: SIMD4(0.2, 0.4, 1, 0.5)),
            CAState.Vertex(0.7, 0.55, u: 1, v: 0, color: SIMD4(1, 1, 1, 0.25)),
        ]
    }

    /// CA's bindings and every other slot its functions read: the transform at vertex buffer 2 (identity, then the
    /// pattern), patterned payloads at vertex 0 and 3 to 5 and fragment 0 to 7, the texture at fragment slots 0 to 15
    /// and the sampler at 0 to 3.
    static func bind(_ encoder: MTLRenderCommandEncoder, source: MTLTexture, sampler: MTLSamplerState) {
        var transform = payload(seed: 2)
        let identity = matrix_identity_float4x4
        withUnsafeBytes(of: identity) { transform.replaceSubrange(0..<64, with: $0) }
        encoder.setVertexBytes(transform, length: transform.count, index: 2)
        for slot in [0, 3, 4, 5] {
            let bytes = payload(seed: UInt32(slot))
            encoder.setVertexBytes(bytes, length: bytes.count, index: slot)
        }
        for slot in 0..<8 {
            let bytes = payload(seed: UInt32(16 + slot))
            encoder.setFragmentBytes(bytes, length: bytes.count, index: slot)
        }
        for slot in 0..<16 { encoder.setFragmentTexture(source, index: slot) }
        for slot in 0..<4 { encoder.setFragmentSamplerState(sampler, index: slot) }
    }

    /// 256 bytes of halves in [0, 1): finite read as half or as float, never a NaN or an infinity.
    static func payload(seed: UInt32) -> [UInt8] {
        var state = seed &* 2_654_435_761 &+ 1
        return (0..<128).flatMap { _ -> [UInt8] in
            state = state &* 1_664_525 &+ 1_013_904_223
            let value = Float16(Float(state >> 8 & 0xffff) / 65536)
            return [UInt8(value.bitPattern & 0xff), UInt8(value.bitPattern >> 8)]
        }
    }

    static func patternTexture(_ gpu: GPU, premultiplied: Bool = false) throws -> MTLTexture {
        let size = 32
        let texture = try gpu.texture(.bgra8Unorm, size, size, usage: [.shaderRead], storage: .shared)
        let texels = (0..<(size * size)).flatMap { i -> [UInt8] in
            let (x, y) = (i % size, i / size)
            let alpha = 128 + (x + y) * 2
            let colour = [x * 8, y * 8, (x ^ y) * 8 & 0xff].map { premultiplied ? $0 * alpha / 255 : $0 }
            return colour.map { UInt8($0) } + [UInt8(alpha)]
        }
        texture.replace(
            region: MTLRegionMake2D(0, 0, size, size), mipmapLevel: 0, withBytes: texels, bytesPerRow: size * 4)
        return texture
    }

    static func linearSampler(_ device: MTLDevice) throws -> MTLSamplerState {
        let descriptor = MTLSamplerDescriptor()
        descriptor.minFilter = .linear
        descriptor.magFilter = .linear
        descriptor.sAddressMode = .clampToEdge
        descriptor.tAddressMode = .clampToEdge
        guard let sampler = device.makeSamplerState(descriptor: descriptor) else {
            throw TestFailure("makeSamplerState returned nil")
        }
        return sampler
    }
}
