import Foundation
import HWTestCore
import Metal

/// 4x MSAA resolved: a triangle with no edge on the pixel grid, each pixel's coverage worked out at the device's own
/// sample positions. Resolved into a texture, into levels 1 and 2 of mipmapped ones and of a format view of one, a slice
/// of a 2D array, a cube's face and a depth plane of a 3D texture; and drawn at one sample into each of those, covered
/// where its centre is. Every level, slice or plane of the destination is first cleared to a colour of its own by a pass
/// into it, and each one the pass did not name must keep it. The rare pixel whose sample lies on an edge (where the fill
/// rule decides) is not checked. On the raw facade every command buffer must run on the raw path.
struct MSAAResolveScene: GPUScene {
    let id = "gpu.msaa.resolve"
    let name = "4x MSAA resolve against sample coverage, into levels, slices and depth planes"

    static let size = 64
    /// Vulkan's and Direct3D's standard 4x positions (Apple's GPUs answer the same), for a device that answers none.
    static let standard: [SIMD2<Double>] = [
        SIMD2(0.375, 0.125), SIMD2(0.875, 0.375), SIMD2(0.125, 0.625), SIMD2(0.625, 0.875),
    ]
    static let centre: [SIMD2<Double>] = [SIMD2(0.5, 0.5)]
    static let triangle: [SIMD2<Double>] = [SIMD2(1101, 947), SIMD2(15130, 2611), SIMD2(5274, 15078)].map { $0 / 256 }

    /// The texture a pass resolves or draws into, and the subresource it names
    enum Destination: CustomStringConvertible {
        case level(Int)
        case viewLevel(Int)
        case slice(Int, of: Int)
        case face(Int)
        case plane(Int, of: Int)

        var description: String {
            switch self {
            case .level(let level): return "level \(level)"
            case .viewLevel(let level): return "level \(level) of a format view"
            case .slice(let slice, let count): return "slice \(slice) of \(count)"
            case .face(let face): return "face \(face)"
            case .plane(let plane, let depth): return "plane \(plane) of \(depth)"
            }
        }

        /// Every (level, slice, plane) of the texture: the one named and the ones the pass must leave alone
        var subresources: [(level: Int, slice: Int, plane: Int)] {
            switch self {
            case .level(let level), .viewLevel(let level): return (0...level).map { ($0, 0, 0) }
            case .slice(_, let count): return (0..<count).map { (0, $0, 0) }
            case .face: return (0..<6).map { (0, $0, 0) }
            case .plane(_, let depth): return (0..<depth).map { (0, 0, $0) }
            }
        }

        var named: (level: Int, slice: Int, plane: Int) {
            switch self {
            case .level(let level), .viewLevel(let level): return (level, 0, 0)
            case .slice(let slice, _), .face(let slice): return (0, slice, 0)
            case .plane(let plane, _): return (0, 0, plane)
            }
        }
    }

    static let destinations: [Destination] = [
        .level(0), .level(1), .level(2), .viewLevel(1), .viewLevel(2), .slice(2, of: 3), .face(4), .plane(2, of: 3),
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        guard gpu.device.supportsTextureSampleCount(4) else {
            result.expect(false, "4x MSAA unsupported")
            return result
        }
        let answered = gpu.device.getDefaultSamplePositions(sampleCount: 4).map {
            SIMD2<Double>(Double($0.x), Double($0.y))
        }
        let usable =
            Set(answered.map { "\($0)" }).count == 4
            && answered.allSatisfy { $0.x >= 0 && $0.x < 1 && $0.y >= 0 && $0.y < 1 }
        result.metrics.set("positions", usable ? "device" : "standard")
        let resolved = Self.coverage(usable ? answered : Self.standard)
        let drawn = Self.coverage(Self.centre)
        for format in [TexelFormat.rgba8Unorm, .rgba16Float] {
            for destination in Self.destinations {
                for samples in [4, 1] {
                    let label = "\(format.rawValue) \(samples == 4 ? "resolve into" : "drawn into") \(destination)"
                    do {
                        try await check(
                            gpu, format, destination, samples: samples, coverage: samples == 4 ? resolved : drawn,
                            label: label, &result)
                    } catch {
                        result.expect(false, "\(label): \(GPU.describe(error))")
                    }
                }
            }
        }
        if RawFacade.kind(of: gpu.device) == "raw" {
            let off = gpu.paths.paths.keys.filter { $0 != "raw" }.sorted()
            result.expect(off.isEmpty, "command buffers off the raw path: \(off.joined(separator: ","))")
            result.metrics.set("route", off.isEmpty ? "raw" : "m2v")
        }
        return result
    }

    private func check(
        _ gpu: GPU, _ format: TexelFormat, _ destination: Destination, samples: Int, coverage: [Int?], label: String,
        _ result: inout SceneResult
    ) async throws {
        let size = Self.size
        let texture = try Self.texture(gpu, format, destination)
        var target = texture
        if case .viewLevel = destination {
            guard let view = texture.makeTextureView(pixelFormat: format.metal) else {
                throw TestFailure("makeTextureView(\(format.rawValue)) returned nil")
            }
            target = view
        }
        let parts = destination.subresources
        try await gpu.run { commands in
            for (index, part) in parts.enumerated() {
                let pass = MTLRenderPassDescriptor()
                Self.name(pass.colorAttachments[0], texture: target, part)
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = Self.marker(index)
                pass.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
        }
        let multisampled =
            samples > 1 ? try gpu.texture(format.metal, size, size, usage: [.renderTarget], samples: 4) : nil
        let pipeline = try gpu.render(fragment: "solid_fragment", format: format.metal, samples: samples)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            let attachment = pass.colorAttachments[0]!
            attachment.loadAction = .clear
            attachment.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            if let multisampled {
                attachment.texture = multisampled
                attachment.storeAction = .multisampleResolve
                attachment.resolveTexture = target
                attachment.resolveLevel = destination.named.level
                attachment.resolveSlice = destination.named.slice
                attachment.resolveDepthPlane = destination.named.plane
            } else {
                Self.name(attachment, texture: target, destination.named)
                attachment.storeAction = .store
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            var corners = Self.triangle.map {
                SIMD4<Float>(Float($0.x / Double(size) * 2 - 1), Float(1 - $0.y / Double(size) * 2), 0.5, 1)
            }
            var white = SIMD4<Float>(1, 1, 1, 1)
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 3, index: 0)
            encoder.setFragmentBytes(&white, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            encoder.endEncoding()
        }
        let tolerance = 1.01 * format.step()
        for (index, part) in parts.enumerated() {
            let got = try await gpu.raster(texture, format, level: part.level, slice: part.slice, plane: part.plane)
            if part == destination.named {
                let want = Raster(width: size, height: size, channels: format.channels, levels: format.levels) {
                    x, y, c in
                    guard let covered = coverage[y * size + x] else { return .nan }
                    return c == 3 ? 1 : Float(covered) / Float(samples)
                }
                result.compare(label, got, want, tolerance: tolerance)
            } else {
                let kept = Self.marker(index)
                let colour = [kept.red, kept.green, kept.blue, kept.alpha].map(Float.init)
                let want = Raster(
                    width: got.width, height: got.height, channels: format.channels, levels: format.levels
                ) { _, _, c in colour[c] }
                result.compare(
                    "\(label): level \(part.level) slice \(part.slice) plane \(part.plane) kept", got, want,
                    tolerance: tolerance)
            }
        }
    }

    /// The destination at `size` x `size` in the subresource it names
    static func texture(_ gpu: GPU, _ format: TexelFormat, _ destination: Destination) throws -> MTLTexture {
        let descriptor = MTLTextureDescriptor()
        descriptor.pixelFormat = format.metal
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .private
        descriptor.width = size
        descriptor.height = size
        switch destination {
        case .level(let level), .viewLevel(let level):
            descriptor.width = size << level
            descriptor.height = size << level
            descriptor.mipmapLevelCount = level + 1
        case .slice(_, let count):
            descriptor.textureType = .type2DArray
            descriptor.arrayLength = count
        case .face:
            descriptor.textureType = .typeCube
        case .plane(_, let depth):
            descriptor.textureType = .type3D
            descriptor.depth = depth
        }
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else {
            throw TestFailure("makeTexture(\(format.rawValue) \(destination)) returned nil")
        }
        return texture
    }

    static func name(
        _ attachment: MTLRenderPassColorAttachmentDescriptor, texture: MTLTexture,
        _ part: (level: Int, slice: Int, plane: Int)
    ) {
        attachment.texture = texture
        attachment.level = part.level
        attachment.slice = part.slice
        attachment.depthPlane = part.plane
    }

    /// A clear colour each subresource starts from, exact in both formats
    static func marker(_ index: Int) -> MTLClearColor {
        MTLClearColor(red: 0.25, green: 0.5 + 0.0625 * Double(index), blue: 0.75, alpha: 1)
    }

    /// Covered samples per pixel at `positions`; nil where a sample sits on an edge.
    static func coverage(_ positions: [SIMD2<Double>]) -> [Int?] {
        let t = triangle
        func edge(_ a: SIMD2<Double>, _ b: SIMD2<Double>, _ p: SIMD2<Double>) -> Double {
            (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)
        }
        return (0..<(size * size)).map { index in
            var covered = 0
            for offset in positions {
                let p = SIMD2<Double>(Double(index % size), Double(index / size)) + offset
                let e = [edge(t[0], t[1], p), edge(t[1], t[2], p), edge(t[2], t[0], p)]
                if e.contains(where: { abs($0) < 1e-9 }) { return nil }
                if e.allSatisfy({ $0 > 0 }) || e.allSatisfy({ $0 < 0 }) { covered += 1 }
            }
            return covered
        }
    }
}
