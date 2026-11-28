import Foundation
import HWTestCore
import Metal

/// The GPU's rates, one unit at a time (shaders/GPURates.metal): FMA issue, copy and read bandwidth by storage and by
/// page spread, colour, blend, 4x MSAA and depth fill, bilinear texels, triangles drawn and clipped, what a depth-hidden
/// layer of an expensive program costs against a shaded one, and a same-address atomic, a vertex fetch and a table read
/// from shared buffers bound L2-cached against L2-bypassing. Each arm is the fastest of `repeats` runs after a warm one,
/// so a run a compositor frame preempted does not count.
///
/// `lane_mhz` is the FMA rate over GM20B's 256 lanes (2 SMMs of 128): the clock the lanes ran at if every cycle issued an
/// FMA. The other arms' `_per_clk` rates divide by it. The kernel's stats line reports the programmed clock.
public final class GPURates {
    public struct Sizes: Sendable {
        public var width = 1280, height = 720, layers = 16
        public var aluThreads = 1 << 18, aluIterations: UInt32 = 256
        public var bytes = 64 << 20, readThreads = 1 << 16
        public var triangles = 1 << 20, heavyIterations: UInt32 = 64
        public var atomics = 65536, fetchTriangles = 1 << 18, smallJobs = 60
        public var fillWidth = 1920, fillHeight = 1080
        public var repeats = 5

        public static let device = Sizes()

        public static let host: Sizes = {
            var s = Sizes()
            s.width = 256
            s.height = 128
            s.layers = 2
            s.aluThreads = 1 << 12
            s.aluIterations = 16
            s.bytes = 1 << 20
            s.readThreads = 1 << 10
            s.triangles = 1 << 12
            s.heavyIterations = 4
            s.atomics = 4096
            s.fetchTriangles = 4096
            s.smallJobs = 8
            s.fillWidth = 256
            s.fillHeight = 128
            s.repeats = 2
            return s
        }()
    }

    static let lanes = 256.0
    static let pageBytes = 4096

    let gpu: GPU
    let s: Sizes
    var metrics = Metrics()
    var problems: [String] = []
    var laneMhz = 0.0
    /// Waits for the facade's compression thread (HWTestKit CompressionSettled.settle); nil off the device
    var settleCompression: (() async throws -> Void)?

    init(_ gpu: GPU, _ s: Sizes) {
        self.gpu = gpu
        self.s = s
    }

    public static func run(_ gpu: GPU, _ s: Sizes = .device) async throws -> SceneResult {
        let b = GPURates(gpu, s)
        var notes: [String] = []
        try await b.alu()
        try await b.bandwidth()
        try await b.fill()
        try await b.texels()
        try await b.triangles()
        try await b.hidden()
        for (mode, bypass) in [("cached", false), ("volatile", true)] {
            guard let made = try b.sharedBuffers(bypass) else {
                notes.append("no \(mode) shared arm: the device cannot make L2-bypassing shared buffers")
                continue
            }
            try await b.sharedArms(mode, made)
        }
        var r = SceneResult()
        r.notes = notes
        r.metrics = b.metrics
        for p in b.problems { r.expect(false, p) }
        return r
    }

    static func rate(_ count: Double, _ ms: Double) -> Double { ms > 0 ? count / (ms / 1000) : 0 }

    func perClock(_ count: Double, _ ms: Double) -> Double {
        laneMhz > 0 ? Self.rate(count, ms) / (laneMhz * 1e6) : 0
    }

    static func group(_ p: MTLComputePipelineState) -> MTLSize {
        MTLSize(width: min(256, p.maxTotalThreadsPerThreadgroup), height: 1, depth: 1)
    }

    /// The warm run, then the fastest of `repeats` GPU times; 0 where a run had no GPU timestamps
    func fastest(_ arm: String, _ encode: (MTLCommandBuffer) throws -> Void) async throws -> Double {
        try await gpu.run(encode)
        var best = Double.infinity
        for _ in 0 ..< s.repeats {
            let t = try await gpu.run(encode)
            guard t.gpuTimed else {
                problems.append("\(arm): no GPU timestamps")
                return 0
            }
            best = min(best, t.gpuMilliseconds)
        }
        return best
    }

    func alu() async throws {
        let fma = try gpu.compute("rates_fma")
        let data = try gpu.buffer([Float](repeating: 0.25, count: s.aluThreads * 4))
        let t = try await fastest("alu") { c in
            var n = self.s.aluIterations
            let e = c.makeComputeCommandEncoder()!
            e.setComputePipelineState(fma)
            e.setBuffer(data, offset: 0, index: 0)
            e.setBytes(&n, length: 4, index: 1)
            e.dispatchThreads(
                MTLSize(width: self.s.aluThreads, height: 1, depth: 1),
                threadsPerThreadgroup: Self.group(fma)
            )
            e.endEncoding()
        }
        let fmas = Double(s.aluThreads) * Double(s.aluIterations) * 8
        laneMhz = Self.rate(fmas, t) / Self.lanes / 1e6
        metrics.set("alu_gflops", Self.rate(2 * fmas, t) / 1e9, digits: 1)
        metrics.set("lane_mhz", laneMhz, digits: 1)
    }

    /// A blit copy between private buffers, and every float4 of a private and of a shared buffer read once, lanes side by
    /// side and lanes a page apart
    func bandwidth() async throws {
        guard let a = gpu.device.makeBuffer(length: s.bytes, options: .storageModePrivate),
              let b = gpu.device.makeBuffer(length: s.bytes, options: .storageModePrivate)
        else { throw TestFailure("no private buffers of \(s.bytes) bytes") }
        let shared = try gpu.buffer(s.bytes)
        let copy = try await fastest("copy") { c in
            let e = c.makeBlitCommandEncoder()!
            e.copy(from: a, sourceOffset: 0, to: b, destinationOffset: 0, size: self.s.bytes)
            e.endEncoding()
        }
        metrics.set("copy_gbps", Self.rate(Double(2 * s.bytes), copy) / 1e9, digits: 2)
        let read = try gpu.compute("rates_read")
        let out = try gpu.buffer(s.readThreads * 16)
        let elements = UInt32(s.bytes / 16), pages = UInt32(s.bytes / Self.pageBytes)
        for (storage, buffer) in [("private", a), ("shared", shared)] {
            for (spread, pageCount) in [("", UInt32(0)), ("_pages", pages)] {
                let arm = "read_gbps_" + storage + spread
                let t = try await fastest(arm) { c in
                    var shape = SIMD2<UInt32>(elements, pageCount)
                    let e = c.makeComputeCommandEncoder()!
                    e.setComputePipelineState(read)
                    e.setBuffer(buffer, offset: 0, index: 0)
                    e.setBuffer(out, offset: 0, index: 1)
                    e.setBytes(&shape, length: 8, index: 2)
                    e.dispatchThreads(
                        MTLSize(width: self.s.readThreads, height: 1, depth: 1), threadsPerThreadgroup: Self.group(read)
                    )
                    e.endEncoding()
                }
                metrics.set(arm, Self.rate(Double(s.bytes), t) / 1e9, digits: 2)
            }
        }
    }

    func pass(_ colour: MTLTexture?, _ depth: MTLTexture?, _ c: MTLCommandBuffer) -> MTLRenderCommandEncoder {
        let pass = MTLRenderPassDescriptor()
        if let colour {
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
        }
        if let depth {
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .store
        }
        return c.makeRenderCommandEncoder(descriptor: pass)!
    }

    func depthState(_ compare: MTLCompareFunction) throws -> MTLDepthStencilState {
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = compare
        d.isDepthWriteEnabled = true
        guard let state = gpu.device.makeDepthStencilState(descriptor: d) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        return state
    }

    /// `layers` full-screen quads a pass, each nearer than the one before: opaque colour, alpha-blended colour, opaque
    /// colour into a shared target, 4x MSAA colour (rated in samples), and depth alone under LESS
    func fill() async throws {
        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let blended = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm) { d in
            let c = d.colorAttachments[0]!
            c.isBlendingEnabled = true
            c.sourceRGBBlendFactor = .sourceAlpha
            c.destinationRGBBlendFactor = .oneMinusSourceAlpha
        }
        let msaa = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm, samples: 4)
        let depthOnly = try gpu.render(fragment: "solid_fragment", format: .invalid, depth: .depth32Float) { d in
            d.fragmentFunction = nil
        }
        let less = try depthState(.less)
        let arms: [(String, Int, MTLTexture?, MTLTexture?, MTLRenderPipelineState)] = [
            ("fill", 1, try gpu.texture(.bgra8Unorm, s.width, s.height), nil, solid),
            ("blend", 1, try gpu.texture(.bgra8Unorm, s.width, s.height), nil, blended),
            ("fill_shared", 1, try gpu.texture(.bgra8Unorm, s.width, s.height, storage: .shared), nil, solid),
            (
                "fill4x",
                4,
                try gpu.texture(.bgra8Unorm, s.width, s.height, usage: [.renderTarget], samples: 4),
                nil,
                msaa
            ),
            ("depth", 1, nil, try gpu.texture(.depth32Float, s.width, s.height, usage: [.renderTarget]), depthOnly),
        ]
        let units = Double(s.width * s.height * s.layers)
        for (name, samples, colour, depth, pipeline) in arms {
            let t = try await fastest(name) { c in
                let e = self.pass(colour, depth, c)
                e.setRenderPipelineState(pipeline)
                if depth != nil { e.setDepthStencilState(less) }
                for i in 0 ..< self.s.layers {
                    let z = 1 - Float(i + 1) / Float(self.s.layers + 1)
                    GPU.draw(e, corners: GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: z), color: SIMD4(0.2, 0.4, 0.6, 0.5))
                }
                e.endEncoding()
            }
            metrics.set(name + "_gpix_s", Self.rate(units * Double(samples), t) / 1e9, digits: 2)
            metrics.set(name + "_per_clk", perClock(units * Double(samples), t), digits: 2)
        }
    }

    func texels() async throws {
        let pipeline = try gpu.render(fragment: "rates_texel_fragment", format: .bgra8Unorm)
        let image = try gpu.texture(.rgba8Unorm, 256, 256, usage: [.shaderRead])
        let bytes = (0 ..< 256 * 256 * 4).map { UInt8(truncatingIfNeeded: ($0 &* 40503) >> 7) }
        try await gpu.upload(bytes, to: image, bytesPerRow: 256 * 4)
        let target = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let t = try await fastest("texel") { c in
            let e = self.pass(target, nil, c)
            e.setRenderPipelineState(pipeline)
            e.setFragmentTexture(image, index: 0)
            var corners = GPU.fullScreen
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }
        let samples = Double(s.width * s.height * 8)
        metrics.set("texel_gtexel_s", Self.rate(samples, t) / 1e9, digits: 2)
        metrics.set("texel_per_clk", perClock(samples, t), digits: 2)
    }

    /// One-pixel triangles, one draw of `triangles`, and the same moved past the target's edge
    func triangles() async throws {
        let pipeline = try gpu.render(vertex: "rates_triangle_vertex", fragment: "solid_fragment", format: .bgra8Unorm)
        let target = try gpu.texture(.bgra8Unorm, s.width, s.height)
        for (name, clipped) in [("tri", UInt32(0)), ("clipped", 1)] {
            let t = try await fastest(name) { c in
                let e = self.pass(target, nil, c)
                e.setRenderPipelineState(pipeline)
                var grid = SIMD4<UInt32>(UInt32(self.s.width), UInt32(self.s.height), clipped, 0)
                var colour = SIMD4<Float>(0.5, 0.5, 0.5, 1)
                e.setVertexBytes(&grid, length: 16, index: 0)
                e.setFragmentBytes(&colour, length: 16, index: 0)
                e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: self.s.triangles * 3)
                e.endEncoding()
            }
            metrics.set(name + "_mtri_s", Self.rate(Double(s.triangles), t) / 1e6, digits: 1)
            metrics.set(name + "_per_clk", perClock(Double(s.triangles), t), digits: 3)
        }
    }

    /// A full-screen quad, then `layers` quads of rates_heavy_fragment under LEQUAL: behind it (hidden), which zcull or
    /// early depth should reject whole, and in front of it (shaded)
    func hidden() async throws {
        let heavy = try gpu.render(fragment: "rates_heavy_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let flat = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let colour = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let depth = try gpu.texture(.depth32Float, s.width, s.height, usage: [.renderTarget])
        let lequal = try depthState(.lessEqual)
        for (name, front) in [("hidden", Float(0.1)), ("shaded", Float(0.9))] {
            let t = try await fastest(name) { c in
                let e = self.pass(colour, depth, c)
                e.setDepthStencilState(lequal)
                e.setRenderPipelineState(flat)
                GPU.draw(e, corners: GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: front), color: SIMD4(0, 0, 0, 1))
                e.setRenderPipelineState(heavy)
                var corners = GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.5)
                var n = self.s.heavyIterations
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                e.setFragmentBytes(&n, length: 4, index: 0)
                for _ in 0 ..< self.s.layers { e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4) }
                e.endEncoding()
            }
            metrics.set(name + "_us_per_layer", t * 1000 / Double(s.layers), digits: 1)
        }
    }

    /// Shared buffers made from now on bypass the GPU's L2 (`on`) or not; false where `on` and the facade cannot (no probe,
    /// or a kernel that takes no volatile writes, where such a buffer would be L2-cached anyway)
    @discardableResult
    public static func bypassL2(_ device: MTLDevice, _ on: Bool) -> Bool {
        let selector = NSSelectorFromString("switchMetalSharedBypassesL2:")
        guard let object = device as? NSObject, object.responds(to: selector) else { return !on }
        let answer = object.perform(selector, with: NSNumber(value: on))?.takeUnretainedValue() as? String
        return !on || answer != "unsupported"
    }

    struct SharedBuffers {
        let counter: MTLBuffer, corners: MTLBuffer, table: MTLBuffer, small: MTLBuffer, smallIn: MTLBuffer
        let smallOut: MTLBuffer
    }

    /// The shared arms' buffers, made L2-bypassing where `bypass`; nil where the device cannot make them so
    func sharedBuffers(_ bypass: Bool) throws -> SharedBuffers? {
        guard Self.bypassL2(gpu.device, bypass) else { return nil }
        defer { if bypass { _ = Self.bypassL2(gpu.device, false) } }
        var corners: [SIMD4<Float>] = []
        corners.reserveCapacity(s.fetchTriangles * 3)
        for t in 0 ..< s.fetchTriangles {
            let pixel = t % (s.width * s.height)
            let x = Float(pixel % s.width), y = Float(pixel / s.width)
            for (dx, dy) in [(Float(0), Float(0)), (1.5, 0), (0, 1.5)] {
                corners.append(
                    SIMD4((x + dx) / Float(s.width) * 2 - 1, 1 - (y + dy) / Float(s.height) * 2, 0.5, 1)
                )
            }
        }
        let table = (0 ..< 256).map { SIMD4<Float>(Float($0) / 255, 0.5, 0.25, 1) }
        return SharedBuffers(
            counter: try gpu.buffer([UInt32(0)]), corners: try gpu.buffer(corners), table: try gpu.buffer(table),
            small: try gpu.buffer(256), smallIn: try gpu.buffer(256), smallOut: try gpu.buffer(256)
        )
    }

    /// One counter every thread adds to, one-pixel triangles whose corners a draw reads, and a fragment program reading a
    /// 4 KiB table sixteen times a pixel, all from shared buffers `mode` (cached or volatile)
    func sharedArms(_ mode: String, _ b: SharedBuffers) async throws {
        let atomic = try gpu.compute("rates_atomic")
        let tAtomic = try await fastest("atomic_" + mode) { c in
            let e = c.makeComputeCommandEncoder()!
            e.setComputePipelineState(atomic)
            e.setBuffer(b.counter, offset: 0, index: 0)
            e.dispatchThreads(
                MTLSize(width: self.s.atomics, height: 1, depth: 1),
                threadsPerThreadgroup: Self.group(atomic)
            )
            e.endEncoding()
        }
        let counted = b.counter.contents().load(as: UInt32.self), want = UInt32(s.atomics * (s.repeats + 1))
        if counted != want { problems.append("atomic_\(mode): the counter read \(counted), not \(want)") }
        metrics.set("atomic_ns_" + mode, tAtomic * 1e6 / Double(s.atomics), digits: 2)
        let target = try gpu.texture(.bgra8Unorm, s.width, s.height)
        let fetch = try gpu.render(vertex: "rates_fetch_vertex", fragment: "solid_fragment", format: .bgra8Unorm)
        let tFetch = try await fastest("fetch_" + mode) { c in
            let e = self.pass(target, nil, c)
            e.setRenderPipelineState(fetch)
            var colour = SIMD4<Float>(0.5, 0.5, 0.5, 1)
            e.setVertexBuffer(b.corners, offset: 0, index: 0)
            e.setFragmentBytes(&colour, length: 16, index: 0)
            e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: self.s.fetchTriangles * 3)
            e.endEncoding()
        }
        metrics.set("fetch_mtri_s_" + mode, Self.rate(Double(s.fetchTriangles), tFetch) / 1e6, digits: 1)
        let table = try gpu.render(fragment: "rates_table_fragment", format: .bgra8Unorm)
        let tTable = try await fastest("table_" + mode) { c in
            let e = self.pass(target, nil, c)
            e.setRenderPipelineState(table)
            var corners = GPU.fullScreen
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setFragmentBuffer(b.table, offset: 0, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }
        metrics.set("table_us_" + mode, tTable * 1000, digits: 1)
        try await smallJobs(mode, b)
    }

    /// `smallJobs` command buffers one after another, each after the CPU rewrote 256 bytes of a shared buffer: a blit
    /// reads them into a private buffer and on into a second shared one, and fills a third. The median
    /// commit-to-completed time is what a job's L2 maintenance (the host-written invalidate, the External flush) costs on
    /// top of its tiny work. Job i must read the CPU's byte i (a miss: it read an old L2 line) and leave its fill in
    /// memory at its completion.
    func smallJobs(_ mode: String, _ b: SharedBuffers) async throws {
        guard let priv = gpu.device.makeBuffer(length: 256, options: .storageModePrivate) else {
            throw TestFailure("no private buffer of 256 bytes")
        }
        var walls: [Double] = []
        for i in 0 ..< s.smallJobs + 1 {
            b.small.contents().initializeMemory(as: UInt8.self, repeating: UInt8(truncatingIfNeeded: i), count: 256)
            let t = try await gpu.run { c in
                let e = c.makeBlitCommandEncoder()!
                e.copy(from: b.small, sourceOffset: 0, to: priv, destinationOffset: 0, size: 256)
                e.copy(from: priv, sourceOffset: 0, to: b.smallIn, destinationOffset: 0, size: 256)
                e.fill(buffer: b.smallOut, range: 0 ..< 256, value: UInt8(truncatingIfNeeded: i))
                e.endEncoding()
            }
            if i > 0 { walls.append(t.wallMilliseconds) }
            if b.smallOut.contents().load(as: UInt8.self) != UInt8(truncatingIfNeeded: i) {
                problems.append("jobs_\(mode): job \(i)'s fill was not in memory at its completion")
            }
            let read = UnsafeBufferPointer(start: b.smallIn.contents().assumingMemoryBound(to: UInt8.self), count: 256)
            if read.contains(where: { $0 != UInt8(truncatingIfNeeded: i) }) {
                problems.append("jobs_\(mode): job \(i) read the shared buffer before the CPU's write of it")
            }
        }
        metrics.set("jobs_us_" + mode, Stats.percentile(walls, 0.5) * 1000, digits: 1)
    }
}
