import CoreVideo
import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

enum PlanarRenderThenSampleCases {
    static let side = 64
    static let copyOrigin = MTLOrigin(x: 5, y: 3, z: 0)
    static let copySize = MTLSize(width: 37, height: 29, depth: 1)

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct PlanarRtsOut { float4 position [[position]]; };
    vertex PlanarRtsOut planar_rts_vertex(uint i [[vertex_id]]) {
        float2 xy = float2((i << 1) & 2, i & 2);
        return {float4(xy * 2 - 1, 0, 1)};
    }
    fragment float4 planar_rts_solid(constant float4 &colour [[buffer(0)]]) { return colour; }
    fragment float4 planar_rts_sample(PlanarRtsOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {
        constexpr sampler s(coord::pixel, filter::nearest, address::clamp_to_edge);
        return t.sample(s, in.position.xy);
    }
    kernel void planar_rts_kernel(texture2d<float> t [[texture(0)]], texture2d<float, access::write> out [[texture(1)]],
                                  uint2 g [[thread_position_in_grid]]) {
        constexpr sampler s(coord::pixel, filter::nearest, address::clamp_to_edge);
        out.write(t.sample(s, float2(g) + 0.5), g);
    }
    """

    typealias Spot = (x: Int, y: Int, rgba: SIMD4<Float>)

    struct Format {
        let format: UInt
        let fourcc: OSType
        let spots: [Spot]

        var key: String { "\(format)" }
    }

    static let sceneSpots: [Spot] = [
        (4, 4, SIMD4(0.248047, 0.501953, 0.747559, 1)), (32, 32, SIMD4(0.897949, 0.201660, 0.102051, 1)),
    ]

    static let formats = [
        Format(format: 502, fourcc: kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange, spots: sceneSpots),
        Format(format: 500, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, spots: sceneSpots),
        Format(format: 540, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, spots: []),
    ]

    enum Draw {
        case scene, partial, other

        var load: MTLLoadAction { self == .partial ? .load : .clear }

        var clear: MTLClearColor {
            self == .other
                ? MTLClearColor(red: 0.6, green: 0.1, blue: 0.3, alpha: 1)
                : MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)
        }

        var quad: (rect: MTLScissorRect, colour: SIMD4<Float>)? {
            switch self {
            case .scene: return (MTLScissorRect(x: 16, y: 16, width: 32, height: 32), SIMD4(0.9, 0.2, 0.1, 1))
            case .partial:
                return (MTLScissorRect(x: 13, y: 7, width: 30, height: 21), SIMD4(0.8813, 0.1371, 0.4519, 1))
            case .other: return nil
            }
        }
    }

    enum Step {
        case draw(Draw), sample(Int), kernel(Int), copy

        var output: Int? {
            switch self {
            case .sample(let k), .kernel(let k): return k
            case .draw, .copy: return nil
            }
        }
    }

    struct Arm {
        let key: String
        var setup: Draw?
        let steps: [Step]
        var after: [Step] = []
        var usage: MTLTextureUsage = [.renderTarget, .shaderRead]
        var spotted = false

        var outputs: Int { (steps + after).compactMap(\.output).count }

        var fresh: [Int] {
            var written = false
            var fresh: [Int] = []
            for step in steps + after {
                if let k = step.output {
                    if written { fresh.append(k) }
                } else {
                    written = true
                }
            }
            return fresh
        }
    }

    static let arms = [
        Arm(key: "render", steps: [.draw(.scene), .sample(0)], spotted: true),
        Arm(key: "load", setup: .scene, steps: [.draw(.partial), .sample(0)]),
        Arm(key: "sample_first", steps: [.sample(0), .draw(.scene)], after: [.sample(1)]),
        Arm(key: "twice", steps: [.draw(.scene), .sample(0), .draw(.other), .sample(1)]),
        Arm(key: "kernel", steps: [.draw(.scene), .kernel(0)]),
        Arm(key: "copy", steps: [.copy, .sample(0)], usage: [.shaderRead]),
    ]

    struct States {
        let solid: MTLRenderPipelineState
        let sample: MTLRenderPipelineState
        let kernel: MTLComputePipelineState
    }

    struct Subject {
        let pixels: CVPixelBuffer
        let texture: MTLTexture
        let outputs: [MTLTexture]
        let bytes: MTLBuffer
    }

    struct ArmResult {
        var off = 0
        var fresh: [Int] = []
        var spotOff: Int?
        var first = "-"

        var unseen: Bool { fresh.contains(0) }
    }

    static func library(_ gpu: GPU) async throws -> MTLLibrary {
        try await gpu.device.makeLibrary(source: source, options: nil)
    }

    static func states(_ gpu: GPU, _ library: MTLLibrary, _ f: Format) async throws -> States {
        func render(_ fragment: String, _ format: MTLPixelFormat) async throws -> MTLRenderPipelineState {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = library.makeFunction(name: "planar_rts_vertex")
            d.fragmentFunction = library.makeFunction(name: fragment)
            d.colorAttachments[0].pixelFormat = format
            return try await gpu.device.makeRenderPipelineState(descriptor: d)
        }
        guard let kernel = library.makeFunction(name: "planar_rts_kernel") else {
            throw TestFailure("no planar_rts_kernel")
        }
        return States(
            solid: try await render("planar_rts_solid", MTLPixelFormat(rawValue: f.format)!),
            sample: try await render("planar_rts_sample", .rgba32Float),
            kernel: try await gpu.device.makeComputePipelineState(function: kernel)
        )
    }

    static func scribble(_ offset: Int) -> UInt8 { UInt8(truncatingIfNeeded: offset * 31 + 7) }

    static func subject(_ gpu: GPU, _ f: Format, usage: MTLTextureUsage, outputs: Int) throws -> Subject {
        guard let pixels = YcbcrCornerTest.pixelBuffer(side, side, f.fourcc),
              let surface = CVPixelBufferGetIOSurface(pixels)?.takeUnretainedValue()
        else { throw TestFailure("no \(f.key) surface") }
        IOSurfaceSetValue(surface, "IOSurfaceYCbCrMatrix" as CFString, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        IOSurfaceLock(surface, [], nil)
        for plane in 0 ..< 2 {
            let count = IOSurfaceGetBytesPerRowOfPlane(surface, plane) * IOSurfaceGetHeightOfPlane(surface, plane)
            let base = IOSurfaceGetBaseAddressOfPlane(surface, plane).bindMemory(to: UInt8.self, capacity: count)
            for i in 0 ..< count { base[i] = scribble(i) }
        }
        let pitch = IOSurfaceGetBytesPerRowOfPlane(surface, 0)
        IOSurfaceUnlock(surface, [], nil)
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: f.format)!, width: side, height: side, mipmapped: false
        )
        d.usage = usage
        guard let texture = gpu.device.makeTexture(descriptor: d, iosurface: surface, plane: 0) else {
            throw TestFailure("no format \(f.format) texture over its surface")
        }
        let targets = try (0 ..< outputs).map { _ in
            try gpu.texture(.rgba32Float, side, side, usage: [.renderTarget, .shaderRead, .shaderWrite])
        }
        let copied = (0 ..< copySize.height).flatMap { k in
            (0 ..< side).map { i in ~scribble((copyOrigin.y + k) * pitch + copyOrigin.x + i) }
        }
        return Subject(pixels: pixels, texture: texture, outputs: targets, bytes: try gpu.buffer(copied))
    }

    static func encode(_ step: Step, _ commands: MTLCommandBuffer, _ s: Subject, _ states: States) {
        switch step {
        case .draw(let draw):
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = s.texture
            pass.colorAttachments[0].loadAction = draw.load
            pass.colorAttachments[0].clearColor = draw.clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if let quad = draw.quad {
                var colour = quad.colour
                encoder.setRenderPipelineState(states.solid)
                encoder.setScissorRect(quad.rect)
                encoder.setFragmentBytes(&colour, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            }
            encoder.endEncoding()
        case .sample(let k):
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = s.outputs[k]
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(states.sample)
            encoder.setFragmentTexture(s.texture, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            encoder.endEncoding()
        case .kernel(let k):
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(states.kernel)
            encoder.setTexture(s.texture, index: 0)
            encoder.setTexture(s.outputs[k], index: 1)
            encoder.dispatchThreads(
                MTLSize(width: side, height: side, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
            )
            encoder.endEncoding()
        case .copy:
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: s.bytes, sourceOffset: 0, sourceBytesPerRow: side, sourceBytesPerImage: side * copySize.height,
                sourceSize: copySize, to: s.texture, destinationSlice: 0, destinationLevel: 0,
                destinationOrigin: copyOrigin
            )
            blit.endEncoding()
        }
    }

    static func sampled(
        _ gpu: GPU, _ states: States, _ f: Format, _ arm: Arm, split: Bool
    ) async throws -> [[UInt8]] {
        let s = try subject(gpu, f, usage: arm.usage, outputs: arm.outputs)
        if let setup = arm.setup { try await gpu.run { encode(.draw(setup), $0, s, states) } }
        if split {
            for step in arm.steps { try await gpu.run { encode(step, $0, s, states) } }
        } else {
            try await gpu.run { commands in
                for step in arm.steps { encode(step, commands, s, states) }
            }
        }
        for step in arm.after { try await gpu.run { encode(step, $0, s, states) } }
        var texels: [[UInt8]] = []
        for target in s.outputs { texels.append(try await gpu.read(target, bytesPerPixel: 16)) }
        withExtendedLifetime(s) {}
        return texels
    }

    static func exercise(_ gpu: GPU, _ states: States, _ f: Format, _ arm: Arm) async throws -> ArmResult {
        let one = try await sampled(gpu, states, f, arm, split: false)
        let split = try await sampled(gpu, states, f, arm, split: true)
        let untouched = Arm(key: "before", setup: arm.setup, steps: [.sample(0)], usage: arm.usage)
        let before = try await sampled(gpu, states, f, untouched, split: false)[0]
        var result = ArmResult()
        result.off = zip(one, split).map { channelsOff($0, $1) }.reduce(0, +)
        for (sample, pair) in zip(one, split).enumerated() where result.first == "-" {
            if let i = stride(from: 0, to: pair.0.count, by: 16)
                .first(where: { pair.0[$0 ..< $0 + 16] != pair.1[$0 ..< $0 + 16] }) {
                let a = pair.0.loaded(as: SIMD4<Float>.self, at: i / 16), b = pair.1.loaded(
                    as: SIMD4<Float>.self,
                    at: i / 16
                )
                result.first = "s\(sample) (\(i / 16 % side),\(i / 16 / side)) one \(a) split \(b)"
            }
        }
        result.fresh = arm.fresh.map { channelsOff(one[$0], before) }
        if arm.spotted && !f.spots.isEmpty { result.spotOff = spotOff(one[0], f.spots) }
        return result
    }

    static func channelsOff(_ a: [UInt8], _ b: [UInt8]) -> Int {
        stride(from: 0, to: a.count, by: 4).filter { a[$0 ..< $0 + 4] != b[$0 ..< $0 + 4] }.count
    }

    static func spotOff(_ texels: [UInt8], _ spots: [Spot]) -> Int {
        spots.map { spot in
            let got = texels.loaded(as: SIMD4<Float>.self, at: spot.y * side + spot.x)
            return (0 ..< 4).filter { !(abs(got[$0] - spot.rgba[$0]) <= 0.001) }.count
        }.reduce(0, +)
    }
}
