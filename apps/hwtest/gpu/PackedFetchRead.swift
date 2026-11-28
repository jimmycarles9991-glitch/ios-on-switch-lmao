import HWTestCore
import Metal

/// Every texel of a 16-bit packed target read by a fragment's framebuffer fetch (shaders/PackedFetchRead.metal), as
/// `float4` or `half4`, against the read m2v's storage form gives (fetch/packed.rs): each channel the `half` nearest
/// code / (2^bits - 1), a channel the format lacks 1. The exact quotient is counted beside it, so a green run tells the
/// two apart.
public enum PackedFetchRead {
    public struct Format: Sendable {
        public let name: String
        public let pixel: MTLPixelFormat
        /// Each channel's bit offset and bits, red to alpha (0 bits: the format has no such channel)
        public let fields: [(at: Int, bits: Int)]
    }

    public static let formats = [
        Format(name: "b5g6r5", pixel: .b5g6r5Unorm, fields: [(11, 5), (5, 6), (0, 5), (0, 0)]),
        Format(name: "bgr5a1", pixel: .bgr5A1Unorm, fields: [(10, 5), (5, 5), (0, 5), (15, 1)]),
    ]

    /// Texels whose read is not the model's (`offHalf`) and not the exact quotient (`offExact`); `first` the first
    /// texel off the model, as "texel:read_want"
    public struct Reading: Sendable {
        public var offHalf = 0, offExact = 0
        public var first = "-"
        public var path = "-"
    }

    static let side = 256

    /// The `f32` bits of the `half` nearest code / top
    public static func halfBits(_ code: Int, _ top: Int) -> UInt32 {
        Float(Float16(Double(code) / Double(top))).bitPattern
    }

    public static func exactBits(_ code: Int, _ top: Int) -> UInt32 {
        Float(Double(code) / Double(top)).bitPattern
    }

    static func lanes(_ format: Format, _ texel: Int, _ bits: (Int, Int) -> UInt32) -> [UInt32] {
        format.fields.map { field in
            field.bits == 0 ? Float(1).bitPattern : bits(texel >> field.at & (1 << field.bits - 1), 1 << field.bits - 1)
        }
    }

    public static func run(_ gpu: GPU, _ format: Format, half: Bool) async throws -> Reading {
        let texels = side * side
        let target = try gpu.texture(format.pixel, side, side)
        let bits = try gpu.texture(.rgba32Uint, side, side)
        let words = try gpu.buffer((0 ..< texels).map { UInt16($0) })
        let state = try gpu.render(fragment: half ? "packed_fetch_read_half" : "packed_fetch_read_float",
                                   format: format.pixel) {
            $0.colorAttachments[1].pixelFormat = .rgba32Uint
        }
        var reading = Reading()
        reading.path = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: words, sourceOffset: 0, sourceBytesPerRow: 2 * side, sourceBytesPerImage: 2 * texels,
                sourceSize: MTLSize(width: side, height: side, depth: 1), to: target, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
            blit.endEncoding()
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[1].texture = bits
            pass.colorAttachments[1].loadAction = .dontCare
            pass.colorAttachments[1].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }.path
        let read = try await gpu.read(bits, bytesPerPixel: 16)
        for texel in 0 ..< texels {
            let got = (0 ..< 4).map { k in
                read.withUnsafeBytes { $0.load(fromByteOffset: 16 * texel + 4 * k, as: UInt32.self) }
            }
            let model = lanes(format, texel, halfBits)
            if got != model {
                reading.offHalf += 1
                if reading.first == "-" {
                    let shown = { (v: [UInt32]) in v.map { String($0, radix: 16) }.joined(separator: ".") }
                    reading.first = "\(texel):\(shown(got))_want_\(shown(model))"
                }
            }
            if got != lanes(format, texel, exactBits) { reading.offExact += 1 }
        }
        return reading
    }
}
