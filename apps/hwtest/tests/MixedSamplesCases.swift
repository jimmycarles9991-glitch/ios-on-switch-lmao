import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The scene of gpu.gm20b.mixed_samples (c38-audit m2v-pass-forms/probe/mixprobe.mm): a one-sample RGBA8Unorm colour
/// beside a 4x Depth32Float, 32x32. The depth is filled by a 4x depth-only pass writing sample s at 1 where bit s of the
/// mask is set, else 0; the mixed pass clears the colour to 0, loads the depth and draws white at z 0.5 under Less with
/// depth writes, under a state of 1 or of 4 raster samples. Samples are read by ms_depth_fragment (side by side in an
/// R32Float, as gpu.gm20b.stored_msaa reads them) and copied out by the blit engine.
enum MixedSamplesCases {
    static let side = 32
    static let hashed: UInt32 = 16
    static let positions: [SIMD2<Float>] = [[0.375, 0.125], [0.875, 0.375], [0.125, 0.625], [0.625, 0.875]]
    static let triangle: [SIMD2<Float>] = [[-0.93, -0.81], [0.87, -0.97], [-0.31, 0.91]]

    struct Run {
        var colour: [UInt8] = []
        var before: [Float] = []
        var after: [Float] = []
    }

    struct Scene {
        let gpu: GPU
        let fill, unpack: MTLRenderPipelineState
        let mixed: [Int: MTLRenderPipelineState]
        let always, less: MTLDepthStencilState

        init(_ gpu: GPU) throws {
            self.gpu = gpu
            fill = try gpu.render(
                fragment: "r5_mask_depth_fragment", format: .invalid, samples: 4, depth: .depth32Float
            )
            var mixed: [Int: MTLRenderPipelineState] = [:]
            for raster in [1, 4] {
                mixed[raster] = try gpu.render(
                    fragment: "solid_fragment", format: .rgba8Unorm, samples: raster, depth: .depth32Float
                )
            }
            self.mixed = mixed
            unpack = try gpu.render(fragment: "ms_depth_fragment", format: .r32Float)
            let d = MTLDepthStencilDescriptor()
            d.isDepthWriteEnabled = true
            d.depthCompareFunction = .always
            let a = gpu.device.makeDepthStencilState(descriptor: d)
            d.depthCompareFunction = .less
            guard let a, let l = gpu.device.makeDepthStencilState(descriptor: d) else {
                throw TestFailure("no depth state")
            }
            (always, less) = (a, l)
        }

        func filled(_ mask: UInt32) async throws -> MTLTexture {
            let depth = try gpu.texture(.depth32Float, side, side, samples: 4)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 0.25
                pass.depthAttachment.storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(fill)
                e.setDepthStencilState(always)
                var corners = GPU.fullScreen
                var mask = mask
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                e.setFragmentBytes(&mask, length: 4, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                e.endEncoding()
            }
            return depth
        }

        func mixedPass(_ colour: MTLTexture, _ depth: MTLTexture, raster: Int, triangle: Bool) async throws {
            let corners = triangle ? MixedSamplesCases.triangle.map { SIMD4($0.x, $0.y, 0.5, 1) } : GPU.fullScreen
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = colour
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .load
                pass.depthAttachment.storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(mixed[raster]!)
                e.setDepthStencilState(less)
                var corners = corners
                var white = SIMD4<Float>(1, 1, 1, 1)
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
                e.setFragmentBytes(&white, length: 16, index: 0)
                e.drawPrimitives(
                    type: triangle ? .triangle : .triangleStrip, vertexStart: 0, vertexCount: corners.count
                )
                e.endEncoding()
            }
        }

        /// Every sample of `depth`, (y * side + x) * 4 + s
        func samples(_ depth: MTLTexture) async throws -> [Float] {
            let out = try gpu.texture(.r32Float, side * 4, side)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = out
                pass.colorAttachments[0].loadAction = .dontCare
                pass.colorAttachments[0].storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(unpack)
                var corners = GPU.fullScreen
                var count = UInt32(4)
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                e.setFragmentBytes(&count, length: 4, index: 0)
                e.setFragmentTexture(depth, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                e.endEncoding()
            }
            let bytes = try await gpu.read(out, bytesPerPixel: 4)
            return (0 ..< side * side * 4).map { bytes.loaded(as: Float.self, at: $0) }
        }

        func run(mask: UInt32, raster: Int, triangle: Bool = false) async throws -> Run {
            let depth = try await filled(mask)
            var run = Run()
            if triangle { run.before = try await samples(depth) }
            let colour = try gpu.texture(.rgba8Unorm, side, side)
            try await mixedPass(colour, depth, raster: raster, triangle: triangle)
            run.colour = try await gpu.read(colour, bytesPerPixel: 4)
            run.after = try await samples(depth)
            return run
        }
    }

    /// r5_mask_depth_fragment's mask of pixel (x, y) for a mask of 16 or more
    static func hashedMask(_ x: Int, _ y: Int) -> UInt32 {
        var h = (UInt32(x) &* 0x9E37_79B1) ^ (UInt32(y) &* 0x85EB_CA77)
        h ^= h >> 15
        h = h &* 0x2C1B_3C6D
        h ^= h >> 12
        return h & 15
    }

    /// Which of a pixel's standard sample positions the triangle covers (analyse.py's model: inside every edge, no tie
    /// rule), bit s for sample s
    static func covered(_ x: Int, _ y: Int) -> UInt32 {
        let window = triangle.map { SIMD2<Float>(($0.x + 1) / 2 * Float(side), (1 - $0.y) / 2 * Float(side)) }
        var bits: UInt32 = 0
        for (s, p) in positions.enumerated() {
            let at = SIMD2<Float>(Float(x), Float(y)) + p
            let edges = (0 ..< 3).map { i -> Float in
                let a = window[i], b = window[(i + 1) % 3]
                return (b.x - a.x) * (at.y - a.y) - (b.y - a.y) * (at.x - a.x)
            }
            if edges.allSatisfy({ $0 > 0 }) || edges.allSatisfy({ $0 < 0 }) { bits |= 1 << s }
        }
        return bits
    }

    static func mask(_ samples: [Float], pixel: Int, equal value: Float) -> UInt32 {
        (0 ..< 4).reduce(0) { $0 | (samples[pixel * 4 + $1] == value ? 1 << $1 : 0) }
    }

    struct Uniform {
        var white = ""
        var wrongSamples: UInt32 = 0
        var grey = 0
        var off: [(mask: Int, white: String)] = []
    }

    /// One state's 16 uniform-mask runs: '1' every pixel white, '0' none, 'm' some; the samples whose depth after is not
    /// 0.5 where the mask passed and 0 where it failed, bit s for sample s
    static func uniform(_ runs: [Run], raster: Int) -> Uniform {
        var u = Uniform()
        for (m, run) in runs.enumerated() {
            let whites = (0 ..< side * side).filter { run.colour[4 * $0] == 255 }.count
            let shown = whites == side * side ? "1" : whites == 0 ? "0" : "m\(whites)"
            u.white += shown.count == 1 ? shown : "m"
            let expected = raster == 1 ? m != 0 : m & 1 != 0
            if shown != (expected ? "1" : "0") { u.off.append((m, shown)) }
            u.grey += run.colour.filter { $0 != 0 && $0 != 255 }.count
            for p in 0 ..< side * side {
                for s in 0 ..< 4 where run.after[p * 4 + s] != (m >> s & 1 != 0 ? 0.5 : 0) { u.wrongSamples |= 1 << s }
            }
        }
        return u
    }

    struct Edge {
        var match = 0, swapped = 0, depthMatch = 0, fillOff = 0, grey = 0
        var rows: [String] = []
    }

    /// The triangle over the hashed masks, each pixel's colour against any(covered and passing) and sample 0's
    /// (covered and passing): `match` pixels as the state's rule (any for 1 raster sample, sample 0 for 4), `swapped` as
    /// the other; per row, each pixel the edge crosses: a any only, s sample 0 only, b both, x neither, upper case where
    /// the samples written (0.5) are not the covered and passing ones
    static func edge(_ run: Run, raster: Int) -> Edge {
        var e = Edge()
        e.grey = run.colour.filter { $0 != 0 && $0 != 255 }.count
        for y in 0 ..< side {
            var row = ""
            for x in 0 ..< side {
                let p = y * side + x
                let passing = mask(run.before, pixel: p, equal: 1)
                for s in 0 ..< 4 where (passing >> s & 1) != (hashedMask(x, y) >> s & 1) { e.fillOff += 1 }
                let cov = covered(x, y)
                let live = cov & passing
                let white = run.colour[4 * p] == 255
                let any = live != 0, first = live & 1 != 0
                let rule = raster == 1 ? any : first, other = raster == 1 ? first : any
                e.match += white == rule ? 1 : 0
                e.swapped += white == other ? 1 : 0
                let depthRight = mask(run.after, pixel: p, equal: 0.5) == live
                e.depthMatch += depthRight ? 1 : 0
                guard cov != 0 && cov != 15 else { continue }
                let code = white == any ? (white == first ? "b" : "a") : (white == first ? "s" : "x")
                row += depthRight ? code : code.uppercased()
            }
            e.rows.append(row)
        }
        return e
    }
}
