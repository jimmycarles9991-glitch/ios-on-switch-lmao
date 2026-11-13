import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// gpu.raw.zeta_colour's cells (ZetaColourTest.swift): a zeta given a pattern by the fixed-function depth and stencil,
/// then its stencil moved by the raw path's stencil plane route (a resolve, or a turned pass's turn copies), then read
/// back by aspect and tested as a zeta. The pattern holds every stencil value and a depth of its own a texel, so a texel
/// the route puts elsewhere in the zeta's memory is seen.
struct ZetaColourCells {
    let gpu: GPU
    let rotated: RotatedStates
    let made = Made()

    /// The pipelines and depth/stencil states the cells draw with, each made once
    final class Made {
        var pipelines: [String: MTLRenderPipelineState] = [:]
        var states: [String: MTLDepthStencilState] = [:]
    }

    /// Texels off the pattern: in the depth or the stencil read back, or in the passes that test the zeta
    struct Count {
        var depth = 0, stencil = 0, tested = 0
        var first: String?

        mutating func add(_ other: Count) {
            depth += other.depth
            stencil += other.stencil
            tested += other.tested
            if first == nil { first = other.first }
        }

        mutating func miss(_ what: String, _ cell: String, _ x: Int, _ y: Int, _ got: String, _ want: String) {
            if first == nil { first = "\(cell) \(what) (\(x),\(y)) \(got)!=\(want)" }
        }
    }

    static func stencil(_ x: Int, _ y: Int, _ seed: UInt32) -> UInt8 {
        UInt8(truncatingIfNeeded: UInt32(x) &* 7 &+ UInt32(y) &* 13 &+ (UInt32(x) >> 3) &* 5 &+ seed)
    }

    static func depth(_ x: Int, _ y: Int, _ seed: UInt32) -> Float {
        Float((UInt32(x) * 37 + UInt32(y) * 101 + seed * 17) % 4093 + 1) / 4096
    }

    static func hasDepth(_ format: MTLPixelFormat) -> Bool { format == .depth32Float_stencil8 }

    /// The pattern drawn into the pass's zeta of `format`: its depth, then a draw a stencil bit
    func pattern(_ e: MTLRenderCommandEncoder, _ format: MTLPixelFormat, samples: Int, seed: UInt32) throws {
        let depth: MTLPixelFormat = Self.hasDepth(format) ? format : .invalid
        var p: [UInt32] = [seed, 0]
        var corners = GPU.fullScreen
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        if Self.hasDepth(format) {
            e.setRenderPipelineState(
                try render(
                    fragment: "zeta_depth_fragment", format: .invalid, samples: samples, depth: depth, stencil: format))
            e.setDepthStencilState(try state(depth: .always, writes: true))
            e.setFragmentBytes(&p, length: 8, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }
        e.setRenderPipelineState(
            try render(
                fragment: "zeta_bit_fragment", format: .invalid, samples: samples, depth: depth, stencil: format))
        e.setStencilReferenceValue(0xFF)
        for k in 0..<8 {
            p[1] = 1 << k
            e.setDepthStencilState(try state(stencil: .always, op: .replace, writeMask: 1 << k))
            e.setFragmentBytes(&p, length: 8, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }
    }

    func state(
        depth: MTLCompareFunction = .always, writes: Bool = false, stencil: MTLCompareFunction? = nil,
        op: MTLStencilOperation = .keep, writeMask: UInt32 = 0
    ) throws -> MTLDepthStencilState {
        let key = "\(depth.rawValue) \(writes) \(stencil.map { "\($0.rawValue)" } ?? "-") \(op.rawValue) \(writeMask)"
        if let held = made.states[key] { return held }
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = depth
        d.isDepthWriteEnabled = writes
        if let stencil {
            let face = MTLStencilDescriptor()
            face.stencilCompareFunction = stencil
            face.depthStencilPassOperation = op
            face.writeMask = writeMask
            d.frontFaceStencil = face
            d.backFaceStencil = face
        }
        guard let state = gpu.device.makeDepthStencilState(descriptor: d) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        made.states[key] = state
        return state
    }

    func render(
        fragment: String, format: MTLPixelFormat, samples: Int = 1, depth: MTLPixelFormat, stencil: MTLPixelFormat,
        writes: Bool = true
    ) throws -> MTLRenderPipelineState {
        let key = "\(fragment) \(format.rawValue) \(samples) \(depth.rawValue) \(stencil.rawValue) \(writes)"
        if let held = made.pipelines[key] { return held }
        let pipeline = try gpu.render(
            fragment: fragment, format: format, samples: samples, depth: depth, stencil: stencil
        ) { if !writes { $0.colorAttachments[0].writeMask = [] } }
        made.pipelines[key] = pipeline
        return pipeline
    }

    /// A one-sample `zeta` given the pattern of `seed`
    func filled(_ zeta: MTLTexture, seed: UInt32) async throws {
        try await gpu.run { commands in
            let e = commands.makeRenderCommandEncoder(descriptor: Self.zetaPass(zeta, load: .clear, store: .store))!
            try pattern(e, zeta.pixelFormat, samples: 1, seed: seed)
            e.endEncoding()
        }
    }

    static func zetaPass(_ zeta: MTLTexture, load: MTLLoadAction, store: MTLStoreAction) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        if hasDepth(zeta.pixelFormat) {
            pass.depthAttachment.texture = zeta
            pass.depthAttachment.loadAction = load
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = store
        }
        pass.stencilAttachment.texture = zeta
        pass.stencilAttachment.loadAction = load
        pass.stencilAttachment.clearStencil = 0
        pass.stencilAttachment.storeAction = store
        return pass
    }

    /// A 4x zeta given the pattern and resolved by sample 0 into a one-sample one that held another pattern: its depth
    /// by the depth resolve, then its stencil by the route
    func resolve(_ format: MTLPixelFormat, _ width: Int, _ height: Int, seed: UInt32) async throws -> Count {
        let source = try gpu.texture(format, width, height, usage: [.renderTarget], samples: 4)
        let into = try gpu.texture(format, width, height, usage: [.renderTarget])
        try await filled(into, seed: seed &+ 101)
        try await gpu.run { commands in
            let pass = Self.zetaPass(source, load: .clear, store: .multisampleResolve)
            if Self.hasDepth(format) {
                pass.depthAttachment.resolveTexture = into
                pass.depthAttachment.depthResolveFilter = .sample0
            }
            pass.stencilAttachment.resolveTexture = into
            pass.stencilAttachment.stencilResolveFilter = .sample0
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            try pattern(e, format, samples: 4, seed: seed)
            e.endEncoding()
        }
        return try await check(into, seed: seed, cell: "resolve_\(Self.name(format))_\(width)x\(height)")
    }

    /// A zeta of the frame beside a turned drawable (rotation 1): given the pattern, loaded into the pass's plane-shaped
    /// stand-in and stored back by the route's turn copies, where a draw of each stencil value marks the drawable, a draw
    /// inverts the stencil and a draw at depth 0.5 under LESS writes it where it is nearer. The drawable holds each frame
    /// texel's stencil; the zeta, read back, the inverted one and the nearer depth.
    func turn(_ format: MTLPixelFormat, _ width: Int, _ height: Int, seed: UInt32) async throws -> Count? {
        let cells = RotatedCells(gpu: gpu, states: rotated, turn: 1, width: width, height: height, aligned: true)
        guard let plane = try cells.makePlane() else { return nil }
        let zeta = try gpu.texture(format, width, height, usage: [.renderTarget])
        try await filled(zeta, seed: seed)
        let depth: MTLPixelFormat = Self.hasDepth(format) ? format : .invalid
        let marks = try render(fragment: "solid_fragment", format: .bgra8Unorm, depth: depth, stencil: format)
        let invert = try render(
            fragment: "solid_fragment", format: .bgra8Unorm, depth: depth, stencil: format, writes: false)
        let equal = try state(stencil: .equal)
        let inverting = try state(stencil: .always, op: .invert, writeMask: 0xFF)
        let nearer = try state(depth: .less, writes: true)
        try await gpu.run { commands in
            let pass = Self.zetaPass(zeta, load: .load, store: .store)
            pass.colorAttachments[0].texture = plane.texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(marks)
            e.setDepthStencilState(equal)
            for v in 0..<256 {
                e.setStencilReferenceValue(UInt32(v))
                GPU.draw(e, corners: GPU.fullScreen, color: SIMD4(Float(v) / 255, 0, 0, 1))
            }
            e.setRenderPipelineState(invert)
            e.setDepthStencilState(inverting)
            GPU.draw(e, corners: GPU.fullScreen, color: SIMD4(0, 0, 0, 0))
            if Self.hasDepth(format) {
                e.setDepthStencilState(nearer)
                GPU.draw(e, corners: GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: Self.nearer), color: SIMD4(0, 0, 0, 0))
            }
            e.endEncoding()
        }
        let cell = "turn_\(Self.name(format))_\(width)x\(height)"
        var count = try await check(zeta, seed: seed, cell: cell, inverted: true, nearer: Self.nearer)
        guard plane.surface.lock(options: .readOnly, seed: nil) == 0 else { throw TestFailure("IOSurfaceLock failed") }
        defer { plane.surface.unlock(options: .readOnly, seed: nil) }
        for y in 0..<height {
            for x in 0..<width {
                let (px, py) = cells.stored(x, y)
                let got = plane.surface.baseAddress.load(
                    fromByteOffset: py * plane.rowBytes + px * 4 + 2, as: UInt8.self)
                let want = Self.stencil(x, y, seed)
                if got != want {
                    count.tested += 1
                    count.miss("drawable", cell, x, y, "\(got)", "\(want)")
                }
            }
        }
        return count
    }

    static let nearer: Float = 0.5

    /// `zeta` against the pattern of `seed` (its stencil inverted where `inverted`, its depth at most `nearer`): its depth
    /// and stencil copied out, then marked by a draw of each stencil value under EQUAL and by the pattern's depth under
    /// EQUAL (a mark where the pattern's depth is what it holds)
    func check(
        _ zeta: MTLTexture, seed: UInt32, cell: String, inverted: Bool = false, nearer: Float = 1
    ) async throws -> Count {
        let w = zeta.width, h = zeta.height, format = zeta.pixelFormat
        var count = Count()
        let want = { (x: Int, y: Int) in inverted ? ~Self.stencil(x, y, seed) : Self.stencil(x, y, seed) }
        let stencils = try await gpu.read(
            zeta, bytesPerPixel: 1, options: Self.hasDepth(format) ? .stencilFromDepthStencil : [])
        for y in 0..<h {
            for x in 0..<w where stencils[y * w + x] != want(x, y) {
                count.stencil += 1
                count.miss("stencil", cell, x, y, "\(stencils[y * w + x])", "\(want(x, y))")
            }
        }
        let marks = try await stencilMarks(zeta)
        for y in 0..<h {
            for x in 0..<w where marks[y * w + x] != UInt32(want(x, y)) {
                count.tested += 1
                count.miss("stencil_equal", cell, x, y, "\(marks[y * w + x])", "\(want(x, y))")
            }
        }
        guard Self.hasDepth(format) else { return count }
        let depths = try await gpu.read(zeta, bytesPerPixel: 4, options: .depthFromDepthStencil)
        let equal = try await depthMarks(zeta, seed: seed)
        for y in 0..<h {
            for x in 0..<w {
                let got = depths.loaded(as: Float.self, at: y * w + x), pattern = Self.depth(x, y, seed)
                let want = min(pattern, nearer)
                if got.bitPattern != want.bitPattern {
                    count.depth += 1
                    count.miss("depth", cell, x, y, "\(got)", "\(want)")
                }
                let mark: UInt32 = pattern <= nearer ? 1 : 0
                if equal[y * w + x] != mark {
                    count.tested += 1
                    count.miss("depth_equal", cell, x, y, "\(equal[y * w + x])", "\(mark)")
                }
            }
        }
        return count
    }

    /// Each texel's stencil as the zeta's stencil test finds it: a draw of each value under EQUAL writes it
    private func stencilMarks(_ zeta: MTLTexture) async throws -> [UInt32] {
        let format = zeta.pixelFormat
        let target = try gpu.texture(.r32Uint, zeta.width, zeta.height, usage: [.renderTarget])
        let pipeline = try render(
            fragment: "integer_uint_fragment", format: .r32Uint, depth: Self.hasDepth(format) ? format : .invalid,
            stencil: format)
        let equal = try state(stencil: .equal)
        try await gpu.run { commands in
            let pass = Self.zetaPass(zeta, load: .load, store: .dontCare)
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 4_294_967_295, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            e.setDepthStencilState(equal)
            var corners = GPU.fullScreen
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            for v in 0..<256 {
                var value = SIMD4<UInt32>(UInt32(v), 0, 0, 0)
                e.setStencilReferenceValue(UInt32(v))
                e.setFragmentBytes(&value, length: 16, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            e.endEncoding()
        }
        return try await words(target)
    }

    /// 1 where the zeta's depth test finds the pattern's depth under EQUAL
    private func depthMarks(_ zeta: MTLTexture, seed: UInt32) async throws -> [UInt32] {
        let target = try gpu.texture(.r32Uint, zeta.width, zeta.height, usage: [.renderTarget])
        let pipeline = try render(
            fragment: "zeta_depth_mark_fragment", format: .r32Uint, depth: zeta.pixelFormat, stencil: zeta.pixelFormat)
        let equal = try state(depth: .equal)
        try await gpu.run { commands in
            let pass = Self.zetaPass(zeta, load: .load, store: .dontCare)
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            e.setDepthStencilState(equal)
            var corners = GPU.fullScreen
            var p: [UInt32] = [seed, 0]
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setFragmentBytes(&p, length: 8, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }
        return try await words(target)
    }

    private func words(_ texture: MTLTexture) async throws -> [UInt32] {
        let bytes = try await gpu.read(texture, bytesPerPixel: 4)
        return (0..<(texture.width * texture.height)).map { bytes.loaded(as: UInt32.self, at: $0) }
    }

    static func name(_ format: MTLPixelFormat) -> String { format == .stencil8 ? "s8" : "d32s8" }
}
