import Foundation
import HWTestCore
import Metal

/// Stored (private) multisampled colour, depth and stencil at 2 and 4 samples, in four parts that fail on their own. Colour:
/// a pass draws a full-target quad and a triangle over it, and resolves (storeAndMultisampleResolve); a later pass loads the
/// samples and only resolves them (multisampleResolve). Depth: a depth-only pass of the triangle, then a pass that loads that
/// depth and draws the triangle and, behind it, the quad, resolving the depth to each pixel's minimum, then two passes that
/// load it and resolve its maximum and sample 0. Stencil: Depth32Float_Stencil8 holds `near` where the triangle covers a
/// sample and `far` behind it, resolved by sample 0 beside the depth's minimum, then by depthResolvedSample beside the depth's
/// minimum, maximum and sample 0, and where the depth is stored unresolved, which takes the last sample (an M-series Mac's
/// GPU does; its target's depth kept); Stencil8's by sample 0 under both filters. Each target's stencil is 0xFF before. Positions: a pass at sample positions of its own. Each sample's colour and
/// depth is read back through texture2d_ms and depth2d_ms, and every reference is the triangle's coverage at the samples'
/// positions, worked out on the CPU. On the raw facade every command buffer must run on the raw path.
struct StoredMSAAScene: GPUScene {
    let id = "gpu.raw.stored_msaa"
    let name = "Stored 2x/4x colour and depth: load, store, texture2d_ms reads, resolves and own sample positions"

    static let size = 16
    static let triangle: [SIMD2<Double>] = [SIMD2(301, 211), SIMD2(3803, 707), SIMD2(1229, 3787)].map { $0 / 256 }
    static let red: [Float] = [1, 0, 0, 1]
    /// Stencil values that between them set and clear every bit
    static let near: UInt32 = 0xA5
    static let far: UInt32 = 0x5A
    static let blue: [Float] = [0, 0, 1, 1]
    /// Vulkan's and Direct3D's standard positions (Apple's GPUs answer the same), for a device that answers none
    static let standard: [Int: [SIMD2<Double>]] = [
        2: [SIMD2(0.75, 0.75), SIMD2(0.25, 0.25)],
        4: [SIMD2(0.375, 0.125), SIMD2(0.875, 0.375), SIMD2(0.125, 0.625), SIMD2(0.625, 0.875)],
    ]
    /// Positions of the scene's own, on Metal's grid of sixteenths
    static let own: [Int: [SIMD2<Double>]] = [
        2: [SIMD2(0.125, 0.875), SIMD2(0.8125, 0.3125)],
        4: [SIMD2(0.0625, 0.0625), SIMD2(0.9375, 0.1875), SIMD2(0.25, 0.8125), SIMD2(0.6875, 0.5)],
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        var counts: [String] = []
        for samples in [2, 4] where gpu.device.supportsTextureSampleCount(samples) {
            counts.append("\(samples)x")
            let (positions, source) = Self.positions(gpu, samples)
            result.metrics.set("positions_\(samples)x", source)
            let cover = Self.coverage(positions)
            do {
                try await colour(gpu, samples, cover, &result)
            } catch {
                result.expect(false, "\(samples)x colour: \(GPU.describe(error))")
            }
            do {
                try await depth(gpu, samples, cover, &result)
            } catch {
                result.expect(false, "\(samples)x depth: \(GPU.describe(error))")
            }
            do {
                try await memorylessDepth(gpu, samples, cover, &result)
            } catch {
                result.expect(false, "\(samples)x memoryless depth: \(GPU.describe(error))")
            }
            do {
                try await stencil(gpu, samples, cover, &result)
            } catch {
                result.expect(false, "\(samples)x stencil: \(GPU.describe(error))")
            }
            guard gpu.device.areProgrammableSamplePositionsSupported else {
                result.notes.append("no programmable sample positions")
                continue
            }
            do {
                try await positioned(gpu, samples, &result)
            } catch {
                result.expect(false, "\(samples)x own positions: \(GPU.describe(error))")
            }
        }
        result.expect(!counts.isEmpty, "the device renders no 2x or 4x texture")
        result.metrics.set("counts", counts.joined(separator: ","))
        if RawFacade.kind(of: gpu.device) == "raw" {
            let off = gpu.paths.paths.keys.filter { $0 != "raw" }.sorted()
            result.expect(off.isEmpty, "command buffers off the raw path: \(off.joined(separator: ","))")
            result.metrics.set("route", off.isEmpty ? "raw" : "m2v")
            var packed: [Bool] = []
            for format in [TexelFormat.rgba8Unorm, .r32Float] {
                let fragment = Self.unpackFragment(format)
                guard let held = RawFacade.packed(try gpu.render(fragment: fragment, format: format.metal)) else {
                    continue
                }
                packed.append(held)
                result.expect(held, "\(fragment): a state made at its creation, not the pack's")
            }
            if !packed.isEmpty {
                result.metrics.set("packed", "\(packed.filter { $0 }.count)/\(packed.count)")
            }
        }
        return result
    }

    private static func unpackFragment(_ format: TexelFormat) -> String {
        format == .r32Float ? "ms_depth_fragment" : "ms_sample_fragment"
    }

    private func colour(_ gpu: GPU, _ samples: Int, _ cover: [[Bool]?], _ result: inout SceneResult) async throws {
        let size = Self.size
        let color = try gpu.texture(.rgba8Unorm, size, size, samples: samples)
        let resolved = try gpu.texture(.rgba8Unorm, size, size)
        let again = try gpu.texture(.rgba8Unorm, size, size)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm, samples: samples)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = color
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 1, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .storeAndMultisampleResolve
            pass.colorAttachments[0].resolveTexture = resolved
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            Self.draw(e, GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.75), Self.blue)
            Self.draw(e, Self.clip(Self.triangle, z: 0.5), Self.red)
            e.endEncoding()
        }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = color
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .multisampleResolve
            pass.colorAttachments[0].resolveTexture = again
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        let mean = Raster(width: size, height: size, channels: 4) { x, y, c in
            guard let s = cover[y * size + x] else { return .nan }
            let k = Float(s.filter { $0 }.count), n = Float(samples)
            return (k * Self.red[c] + (n - k) * Self.blue[c]) / n
        }
        result.compare(
            "\(samples)x samples", try await Self.unpack(gpu, color, samples, .rgba8Unorm),
            Self.each(cover, samples, channels: 4) { $0 ? Self.red[$1] : Self.blue[$1] }, tolerance: 0.5 / 255)
        result.compare(
            "\(samples)x storeAndMultisampleResolve", try await gpu.raster(resolved, .rgba8Unorm), mean,
            tolerance: 1.01 / 255)
        result.compare(
            "\(samples)x load then multisampleResolve", try await gpu.raster(again, .rgba8Unorm), mean,
            tolerance: 1.01 / 255)
    }

    private func depth(_ gpu: GPU, _ samples: Int, _ cover: [[Bool]?], _ result: inout SceneResult) async throws {
        let size = Self.size
        let color = try gpu.texture(.rgba8Unorm, size, size, samples: samples)
        let depth = try gpu.texture(.depth32Float, size, size, samples: samples)
        let depthOnly = try gpu.render(
            fragment: "solid_fragment", format: .invalid, samples: samples, depth: .depth32Float)
        let drawn = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, samples: samples, depth: .depth32Float)
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.depthCompareFunction = .lessEqual
        descriptor.isDepthWriteEnabled = true
        guard let lessEqual = gpu.device.makeDepthStencilState(descriptor: descriptor) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        let triangle = Self.clip(Self.triangle, z: 0.5)
        let filters: [MTLMultisampleDepthResolveFilter] = [.min, .max, .sample0]
        let resolved = try filters.map { _ in try gpu.texture(.depth32Float, size, size, usage: [.renderTarget]) }
        try await gpu.run { commands in
            let first = MTLRenderPassDescriptor()
            first.depthAttachment.texture = depth
            first.depthAttachment.loadAction = .clear
            first.depthAttachment.clearDepth = 1
            first.depthAttachment.storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: first)!
            e.setRenderPipelineState(depthOnly)
            e.setDepthStencilState(lessEqual)
            Self.draw(e, triangle, Self.red)
            e.endEncoding()
            let second = MTLRenderPassDescriptor()
            second.colorAttachments[0].texture = color
            second.colorAttachments[0].loadAction = .clear
            second.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 1, blue: 0, alpha: 1)
            second.colorAttachments[0].storeAction = .store
            second.depthAttachment.texture = depth
            second.depthAttachment.loadAction = .load
            second.depthAttachment.storeAction = .storeAndMultisampleResolve
            second.depthAttachment.resolveTexture = resolved[0]
            second.depthAttachment.depthResolveFilter = filters[0]
            let f = commands.makeRenderCommandEncoder(descriptor: second)!
            f.setRenderPipelineState(drawn)
            f.setDepthStencilState(lessEqual)
            Self.draw(f, triangle, Self.red)
            Self.draw(f, GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.75), Self.blue)
            f.endEncoding()
            for k in 1..<filters.count {
                let again = MTLRenderPassDescriptor()
                again.depthAttachment.texture = depth
                again.depthAttachment.loadAction = .load
                again.depthAttachment.storeAction = .storeAndMultisampleResolve
                again.depthAttachment.resolveTexture = resolved[k]
                again.depthAttachment.depthResolveFilter = filters[k]
                commands.makeRenderCommandEncoder(descriptor: again)!.endEncoding()
            }
        }
        for (k, filter) in filters.enumerated() {
            let name = ["sample 0", "minimum", "maximum"][Int(filter.rawValue)]
            result.compare(
                "\(samples)x depth resolved to the \(name)", try await gpu.raster(resolved[k], .r32Float),
                Raster(width: size, height: size, channels: 1, levels: 1) { x, y, _ in
                    guard let s = cover[y * size + x] else { return .nan }
                    let depths = s.map { Float($0 ? 0.5 : 0.75) }
                    return filter == .min ? depths.min()! : filter == .max ? depths.max()! : depths[0]
                }, tolerance: 0)
        }
        result.compare(
            "\(samples)x depth-tested samples", try await Self.unpack(gpu, color, samples, .rgba8Unorm),
            Self.each(cover, samples, channels: 4) { $0 ? Self.red[$1] : Self.blue[$1] }, tolerance: 0.5 / 255)
        result.compare(
            "\(samples)x depth samples", try await Self.unpack(gpu, depth, samples, .r32Float),
            Self.each(cover, samples, channels: 1) { covered, _ in covered ? 0.5 : 0.75 }, tolerance: 0)
    }

    /// A memoryless multisampled depth texture of each format, depth-tested and resolved to its minimum in the pass that
    /// draws it: the raw path's backing for it may be compressed memory, which the resolve samples through a header of its
    /// own
    private func memorylessDepth(
        _ gpu: GPU, _ samples: Int, _ cover: [[Bool]?], _ result: inout SceneResult
    )
        async throws
    {
        let size = Self.size
        for (format, texel, name) in [
            (MTLPixelFormat.depth16Unorm, TexelFormat.depth16Unorm, "Depth16Unorm"),
            (.depth32Float, .depth32Float, "Depth32Float"),
        ] {
            let color = try gpu.texture(.rgba8Unorm, size, size, samples: samples)
            let depth = try gpu.texture(
                format, size, size, usage: [.renderTarget], samples: samples, storage: .memoryless)
            let resolved = try gpu.texture(format, size, size, usage: [.renderTarget, .shaderRead])
            let drawn = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm, samples: samples, depth: format)
            let descriptor = MTLDepthStencilDescriptor()
            descriptor.depthCompareFunction = .lessEqual
            descriptor.isDepthWriteEnabled = true
            guard let lessEqual = gpu.device.makeDepthStencilState(descriptor: descriptor) else {
                throw TestFailure("makeDepthStencilState returned nil")
            }
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = color
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 1
                pass.depthAttachment.storeAction = .multisampleResolve
                pass.depthAttachment.resolveTexture = resolved
                pass.depthAttachment.depthResolveFilter = .min
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(drawn)
                e.setDepthStencilState(lessEqual)
                Self.draw(e, Self.clip(Self.triangle, z: 0.5), Self.red)
                Self.draw(e, GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.75), Self.blue)
                e.endEncoding()
            }
            let code = { (d: Float) in format == .depth16Unorm ? (d * 65535).rounded() / 65535 : d }
            result.compare(
                "\(samples)x memoryless \(name) depth resolved to the minimum", try await gpu.raster(resolved, texel),
                Raster(width: size, height: size, channels: 1, levels: 1) { x, y, _ in
                    guard let s = cover[y * size + x] else { return .nan }
                    return code(s.contains(true) ? 0.5 : 0.75)
                }, tolerance: format == .depth16Unorm ? 1.01 / 65535 : 0)
        }
    }

    private func stencil(_ gpu: GPU, _ samples: Int, _ cover: [[Bool]?], _ result: inout SceneResult) async throws {
        let size = Self.size
        let zeta = try gpu.texture(.depth32Float_stencil8, size, size, usage: [.renderTarget], samples: samples)
        let resolved = try (0..<5).map { _ in
            try gpu.texture(.depth32Float_stencil8, size, size, usage: [.renderTarget])
        }
        let s8 = try gpu.texture(.stencil8, size, size, usage: [.renderTarget], samples: samples)
        let s8Resolved = try (0..<2).map { _ in try gpu.texture(.stencil8, size, size, usage: [.renderTarget]) }
        let tested = try gpu.render(
            fragment: "solid_fragment", format: .invalid, samples: samples, depth: .depth32Float_stencil8,
            stencil: .depth32Float_stencil8)
        let stencilOnly = try gpu.render(
            fragment: "solid_fragment", format: .invalid, samples: samples, stencil: .stencil8)
        let replaceNearer = try Self.stencilWrites(gpu, depth: .lessEqual)
        let replace = try Self.stencilWrites(gpu, depth: .always)
        try await Self.prefill(gpu, resolved + s8Resolved)
        try await gpu.run { commands in
            let first = Self.zetaPass(zeta, load: .clear)
            first.depthAttachment.storeAction = .storeAndMultisampleResolve
            first.depthAttachment.resolveTexture = resolved[0]
            first.depthAttachment.depthResolveFilter = .min
            first.stencilAttachment.storeAction = .storeAndMultisampleResolve
            first.stencilAttachment.resolveTexture = resolved[0]
            first.stencilAttachment.stencilResolveFilter = .sample0
            let e = commands.makeRenderCommandEncoder(descriptor: first)!
            e.setRenderPipelineState(tested)
            e.setDepthStencilState(replaceNearer)
            e.setStencilReferenceValue(Self.near)
            Self.draw(e, Self.clip(Self.triangle, z: 0.5), Self.red)
            e.setStencilReferenceValue(Self.far)
            Self.draw(e, GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.75), Self.blue)
            e.endEncoding()
            let least = Self.zetaPass(zeta, load: .load)
            least.depthAttachment.storeAction = .storeAndMultisampleResolve
            least.depthAttachment.resolveTexture = resolved[1]
            least.depthAttachment.depthResolveFilter = .min
            least.stencilAttachment.storeAction = .storeAndMultisampleResolve
            least.stencilAttachment.resolveTexture = resolved[1]
            least.stencilAttachment.stencilResolveFilter = .depthResolvedSample
            commands.makeRenderCommandEncoder(descriptor: least)!.endEncoding()
            let zero = Self.zetaPass(zeta, load: .load)
            zero.depthAttachment.storeAction = .storeAndMultisampleResolve
            zero.depthAttachment.resolveTexture = resolved[4]
            zero.depthAttachment.depthResolveFilter = .sample0
            zero.stencilAttachment.storeAction = .storeAndMultisampleResolve
            zero.stencilAttachment.resolveTexture = resolved[4]
            zero.stencilAttachment.stencilResolveFilter = .depthResolvedSample
            commands.makeRenderCommandEncoder(descriptor: zero)!.endEncoding()
            let unresolved = Self.zetaPass(zeta, load: .load)
            unresolved.depthAttachment.storeAction = .store
            unresolved.depthAttachment.depthResolveFilter = .min
            unresolved.stencilAttachment.storeAction = .storeAndMultisampleResolve
            unresolved.stencilAttachment.resolveTexture = resolved[3]
            unresolved.stencilAttachment.stencilResolveFilter = .depthResolvedSample
            commands.makeRenderCommandEncoder(descriptor: unresolved)!.endEncoding()
            let greatest = Self.zetaPass(zeta, load: .load)
            greatest.depthAttachment.storeAction = .multisampleResolve
            greatest.depthAttachment.resolveTexture = resolved[2]
            greatest.depthAttachment.depthResolveFilter = .max
            greatest.stencilAttachment.storeAction = .multisampleResolve
            greatest.stencilAttachment.resolveTexture = resolved[2]
            greatest.stencilAttachment.stencilResolveFilter = .depthResolvedSample
            commands.makeRenderCommandEncoder(descriptor: greatest)!.endEncoding()
            let stencil8 = Self.zetaPass(s8, load: .clear)
            stencil8.stencilAttachment.storeAction = .storeAndMultisampleResolve
            stencil8.stencilAttachment.resolveTexture = s8Resolved[0]
            let f = commands.makeRenderCommandEncoder(descriptor: stencil8)!
            f.setRenderPipelineState(stencilOnly)
            f.setDepthStencilState(replace)
            f.setStencilReferenceValue(Self.far)
            Self.draw(f, GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.75), Self.blue)
            f.setStencilReferenceValue(Self.near)
            Self.draw(f, Self.clip(Self.triangle, z: 0.5), Self.red)
            f.endEncoding()
            let again = Self.zetaPass(s8, load: .load)
            again.stencilAttachment.storeAction = .multisampleResolve
            again.stencilAttachment.resolveTexture = s8Resolved[1]
            again.stencilAttachment.stencilResolveFilter = .depthResolvedSample
            commands.makeRenderCommandEncoder(descriptor: again)!.endEncoding()
        }
        let picks: [(String, MTLTexture, ([Bool]) -> Bool)] = [
            ("sample 0", resolved[0], { $0[0] }),
            ("least depth's sample", resolved[1], { $0.contains(true) }),
            ("greatest depth's sample", resolved[2], { $0.allSatisfy { $0 } }),
            ("depth's sample 0", resolved[4], { $0[0] }),
            ("last sample where the depth stores unresolved", resolved[3], { $0[$0.count - 1] }),
            ("Stencil8 sample 0", s8Resolved[0], { $0[0] }),
            ("Stencil8 depthResolvedSample", s8Resolved[1], { $0[0] }),
        ]
        for (name, texture, nearer) in picks {
            result.compare(
                "\(samples)x stencil resolved to the \(name)", try await Self.stencils(gpu, texture),
                Raster(width: size, height: size, channels: 1, levels: 1) { x, y, _ in
                    guard let s = cover[y * size + x] else { return .nan }
                    return Float(nearer(s) ? Self.near : Self.far)
                }, tolerance: 0)
        }
        let kept = try await gpu.read(resolved[3], bytesPerPixel: 4, options: .depthFromDepthStencil)
        result.compare(
            "\(samples)x depth kept beside a stencil resolve", Self.floats(kept),
            Raster(width: size, height: size, channels: 1, levels: 1) { _, _, _ in 0.25 }, tolerance: 0)
    }

    private func positioned(_ gpu: GPU, _ samples: Int, _ result: inout SceneResult) async throws {
        let size = Self.size
        let own = Self.own[samples]!
        let color = try gpu.texture(.rgba8Unorm, size, size, samples: samples)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm, samples: samples)
        let triangle = Self.clip(Self.triangle, z: 0.5)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = color
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 1, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            pass.setSamplePositions(own.map { MTLSamplePosition(x: Float($0.x), y: Float($0.y)) })
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            Self.draw(e, triangle, Self.red)
            e.endEncoding()
        }
        let cover = Self.coverage(own)
        let (standard, _) = Self.positions(gpu, samples)
        let moved = zip(cover, Self.coverage(standard)).contains { $0.0 != nil && $0.1 != nil && $0.0! != $0.1! }
        result.expect(moved, "\(samples)x: the own positions cover what the device's do")
        result.compare(
            "\(samples)x own sample positions", try await Self.unpack(gpu, color, samples, .rgba8Unorm),
            Self.each(cover, samples, channels: 4) { $0 ? Self.red[$1] : Self.blue[$1] }, tolerance: 0.5 / 255)
    }

    /// Each sample of `texture` side by side, sample s of pixel (x, y) at (x * samples + s, y), read by texture2d_ms (or
    /// depth2d_ms) in a fragment function
    private static func unpack(
        _ gpu: GPU, _ texture: MTLTexture, _ samples: Int, _ format: TexelFormat
    ) async throws
        -> Raster
    {
        let out = try gpu.texture(format.metal, size * samples, size)
        let pipeline = try gpu.render(fragment: unpackFragment(format), format: format.metal)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = out
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            var corners = GPU.fullScreen
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
            var count = UInt32(samples)
            e.setFragmentBytes(&count, length: 4, index: 0)
            e.setFragmentTexture(texture, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }
        return try await gpu.raster(out, format)
    }

    /// The samples side by side as `unpack` reads them, each sample's channel `c` the value `fill(covered, c)` gives
    private static func each(_ cover: [[Bool]?], _ samples: Int, channels: Int, _ fill: (Bool, Int) -> Float) -> Raster
    {
        Raster(width: size * samples, height: size, channels: channels, levels: channels == 1 ? 1 : 255) { x, y, c in
            guard let s = cover[y * size + x / samples] else { return .nan }
            return fill(s[x % samples], c)
        }
    }

    /// Each texture's stencil 0xFF and its depth, where it has depth, 0.25
    private static func prefill(_ gpu: GPU, _ textures: [MTLTexture]) async throws {
        try await gpu.run { commands in
            for texture in textures {
                let pass = MTLRenderPassDescriptor()
                if texture.pixelFormat != .stencil8 {
                    pass.depthAttachment.texture = texture
                    pass.depthAttachment.loadAction = .clear
                    pass.depthAttachment.clearDepth = 0.25
                    pass.depthAttachment.storeAction = .store
                }
                pass.stencilAttachment.texture = texture
                pass.stencilAttachment.loadAction = .clear
                pass.stencilAttachment.clearStencil = 0xFF
                pass.stencilAttachment.storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
        }
    }

    /// A pass of `zeta` alone, each of its aspects loaded as `load` (a clear to depth 1 and stencil 0)
    private static func zetaPass(_ zeta: MTLTexture, load: MTLLoadAction) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        if zeta.pixelFormat != .stencil8 {
            pass.depthAttachment.texture = zeta
            pass.depthAttachment.loadAction = load
            pass.depthAttachment.clearDepth = 1
        }
        pass.stencilAttachment.texture = zeta
        pass.stencilAttachment.loadAction = load
        pass.stencilAttachment.clearStencil = 0
        return pass
    }

    /// Stencil Replace on both faces under depth compare `depth` (writing depth unless Always)
    private static func stencilWrites(_ gpu: GPU, depth: MTLCompareFunction) throws -> MTLDepthStencilState {
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = .always
        face.depthStencilPassOperation = .replace
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.depthCompareFunction = depth
        descriptor.isDepthWriteEnabled = depth != .always
        descriptor.frontFaceStencil = face
        descriptor.backFaceStencil = face
        guard let state = gpu.device.makeDepthStencilState(descriptor: descriptor) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        return state
    }

    private static func stencils(_ gpu: GPU, _ texture: MTLTexture) async throws -> Raster {
        let options: MTLBlitOption = texture.pixelFormat == .stencil8 ? [] : .stencilFromDepthStencil
        let bytes = try await gpu.read(texture, bytesPerPixel: 1, options: options)
        return Raster(width: size, height: size, channels: 1, levels: 1) { x, y, _ in Float(bytes[y * size + x]) }
    }

    private static func floats(_ bytes: [UInt8]) -> Raster {
        Raster(width: size, height: size, channels: 1, levels: 1) { x, y, _ in
            bytes.withUnsafeBytes { $0.load(fromByteOffset: (y * size + x) * 4, as: Float.self) }
        }
    }

    private static func draw(_ encoder: MTLRenderCommandEncoder, _ corners: [SIMD4<Float>], _ color: [Float]) {
        var corners = corners
        var color = SIMD4<Float>(color[0], color[1], color[2], color[3])
        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
        encoder.setFragmentBytes(&color, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
        encoder.drawPrimitives(
            type: corners.count == 3 ? .triangle : .triangleStrip, vertexStart: 0, vertexCount: corners.count)
    }

    private static func clip(_ points: [SIMD2<Double>], z: Float) -> [SIMD4<Float>] {
        points.map { SIMD4<Float>(Float($0.x / Double(size) * 2 - 1), Float(1 - $0.y / Double(size) * 2), z, 1) }
    }

    /// The device's own positions where it answers them, else the standard ones
    private static func positions(_ gpu: GPU, _ samples: Int) -> ([SIMD2<Double>], String) {
        let answered = gpu.device.getDefaultSamplePositions(sampleCount: samples).map {
            SIMD2<Double>(Double($0.x), Double($0.y))
        }
        let usable =
            Set(answered.map { "\($0)" }).count == samples
            && answered.allSatisfy { $0.x >= 0 && $0.x < 1 && $0.y >= 0 && $0.y < 1 }
        return usable ? (answered, "device") : (standard[samples]!, "standard")
    }

    /// Each pixel's samples the triangle covers, at `positions`; nil where a sample lies on an edge (the fill rule's)
    private static func coverage(_ positions: [SIMD2<Double>]) -> [[Bool]?] {
        let t = triangle
        func edge(_ a: SIMD2<Double>, _ b: SIMD2<Double>, _ p: SIMD2<Double>) -> Double {
            (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)
        }
        return (0..<(size * size)).map { index in
            var covered: [Bool] = []
            for offset in positions {
                let p = SIMD2<Double>(Double(index % size), Double(index / size)) + offset
                let e = [edge(t[0], t[1], p), edge(t[1], t[2], p), edge(t[2], t[0], p)]
                if e.contains(where: { abs($0) < 1e-9 }) { return nil }
                covered.append(e.allSatisfy { $0 > 0 } || e.allSatisfy { $0 < 0 })
            }
            return covered
        }
    }
}
