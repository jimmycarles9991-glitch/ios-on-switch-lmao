import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.private_compression's shape arms (raw_private_comp.h privateShapeCompresses): a private target of each shape
/// the device lays out on compressed memory, made until it binds some, drawn and read whole, against the same work with
/// compression held off. `<arm>_made` is its comptags when made, `<arm>_compressed` after the work, `<arm>_paths` the
/// command buffers' paths; it fails where the bytes differ, the target was not compressed or the work left the raw path.
/// The mips arm draws into level 1 after generating the levels, the 3D engine's target there a level of the chain.
extension PrivateCompressionTest {
    struct ShapeRun {
        var bytes: [UInt8] = []
        var made = 0, compressed = 0
        var paths: [String] = []
        var musts: [(String, Int)] = []  // facts of the compressed run that must read 1
    }

    static let moveSelector = NSSelectorFromString("switchRawProbeMovePlain:")

    static let shapeArms = ["mips", "array", "ms4", "shader_write", "view_id"]
    /// Clear colours the boot ZBC table holds (black, white, zero), one a slice
    static let sliceClears = [
        MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1), MTLClearColor(red: 1, green: 1, blue: 1, alpha: 1),
        MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0),
    ]

    func shapeArms(_ gpu: GPU, _ facade: NSObject, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        for arm in Self.shapeArms {
            let run = try await shapeArm(arm, gpu, facade, plain: false)
            _ = facade.perform(Self.plainSelector, with: NSNumber(value: true))
            let reference: ShapeRun
            do {
                defer { _ = facade.perform(Self.plainSelector, with: NSNumber(value: false)) }
                reference = try await shapeArm(arm, gpu, facade, plain: true)
            }
            let differ =
                zip(run.bytes, reference.bytes).filter { $0 != $1 }.count + abs(run.bytes.count - reference.bytes.count)
            metrics.set("\(arm)_bytes_differ", differ)
            metrics.set("\(arm)_made", run.made)
            metrics.set("\(arm)_compressed", run.compressed)
            metrics.set("\(arm)_paths", run.paths.joined(separator: "/"))
            if differ > 0 { wrong.append("\(arm)_bytes_differ") }
            if run.made != 1 || run.compressed != 1 { wrong.append("\(arm)_not_compressed") }
            if reference.made == 1 { wrong.append("\(arm)_reference_compressed") }
            if run.paths.contains(where: { $0 != "raw" }) { wrong.append("\(arm)_path") }
            for (name, value) in run.musts {
                metrics.set("\(arm)_\(name)", value)
                if value != 1 { wrong.append("\(arm)_\(name)") }
            }
        }
    }

    /// Every sample of a multisampled BGRA8 `texture`, the samples of a pixel side by side (ms_sample_fragment)
    func sampleGrid(_ gpu: GPU, _ texture: MTLTexture, _ run: inout ShapeRun) async throws -> [UInt8] {
        let count = texture.sampleCount
        let grid = try gpu.texture(.bgra8Unorm, texture.width * count, texture.height)
        let state = try gpu.render(fragment: "ms_sample_fragment", format: .bgra8Unorm)
        run.paths.append(
            try await gpu.run { commands in
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = grid
                p.colorAttachments[0].loadAction = .dontCare
                p.colorAttachments[0].storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: p)!
                e.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                var samples = UInt32(count)
                e.setFragmentBytes(&samples, length: 4, index: 0)
                e.setFragmentTexture(texture, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                e.endEncoding()
            }.path)
        return try await gpu.read(grid, bytesPerPixel: 4)
    }

    /// `make` until what it made binds compressed memory (20 tries), or once where `plain`
    func made(_ facade: NSObject, plain: Bool, _ make: () throws -> MTLTexture) async throws -> MTLTexture {
        var made = try make()
        for _ in 0..<20 where !plain && comptags(facade, made) == 0 {
            try await Task.sleep(nanoseconds: 50_000_000)
            made = try make()
        }
        return made
    }

    func shapeArm(_ arm: String, _ gpu: GPU, _ facade: NSObject, plain: Bool) async throws -> ShapeRun {
        var run = ShapeRun()
        let pattern = try gpu.render(fragment: "comp_pattern_fragment", format: .bgra8Unorm)
        /// The pattern over all of `target` (`half`: its left half, the slice's own clear colour, or `clear`'s, beside it;
        /// `slant`: a left part whose right edge crosses pixels, so their samples differ)
        func draw(
            _ target: MTLTexture, slice: Int = 0, level: Int = 0, half: Bool = false, slant: Bool = false,
            state: MTLRenderPipelineState? = nil, clear: Int? = nil
        ) -> (MTLCommandBuffer) -> Void {
            { commands in
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = target
                p.colorAttachments[0].slice = slice
                p.colorAttachments[0].level = level
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].clearColor = Self.sliceClears[(clear ?? slice) % Self.sliceClears.count]
                p.colorAttachments[0].storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: p)!
                e.setRenderPipelineState(state ?? pattern)
                var corners =
                    slant
                    ? [
                        SIMD4<Float>(-1, -1, 0.5, 1), SIMD4(0.3, -1, 0.5, 1), SIMD4(-1, 1, 0.5, 1),
                        SIMD4(-0.2, 1, 0.5, 1),
                    ]
                    : half ? GPU.quad(x0: -1, y0: -1, x1: 0, y1: 1, z: 0.5) : GPU.fullScreen
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                e.endEncoding()
            }
        }
        switch arm {
        case "mips":
            let target = try await made(facade, plain: plain) { try gpu.texture(.bgra8Unorm, 256, 256, mipmaps: 5) }
            run.made = comptags(facade, target)
            run.paths.append(try await gpu.run(draw(target)).path)
            run.paths.append(
                try await gpu.run { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.generateMipmaps(for: target)
                    blit.endEncoding()
                }.path)
            run.paths.append(try await gpu.run(draw(target, level: 1, half: true, clear: 1)).path)
            for level in 0..<target.mipmapLevelCount {
                run.bytes += try await gpu.read(target, bytesPerPixel: 4, level: level)
            }
            run.compressed = comptags(facade, target)
        case "array":
            let target = try await made(facade, plain: plain) {
                let d = MTLTextureDescriptor.texture2DDescriptor(
                    pixelFormat: .bgra8Unorm, width: 128, height: 128, mipmapped: false)
                d.textureType = .type2DArray
                d.arrayLength = 3
                d.storageMode = .private
                d.usage = [.renderTarget, .shaderRead]
                guard let t = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no 2D array made") }
                return t
            }
            run.made = comptags(facade, target)
            for slice in 0..<3 { run.paths.append(try await gpu.run(draw(target, slice: slice, half: true)).path) }
            for slice in 0..<3 { run.bytes += try await gpu.read(target, bytesPerPixel: 4, slice: slice) }
            run.compressed = comptags(facade, target)
        case "ms4":
            let target = try await made(facade, plain: plain) { try gpu.texture(.bgra8Unorm, 64, 64, samples: 4) }
            run.made = comptags(facade, target)
            let state = try gpu.render(fragment: "comp_pattern_fragment", format: .bgra8Unorm, samples: 4)
            run.paths.append(try await gpu.run(draw(target, slant: true, state: state)).path)
            run.bytes = try await sampleGrid(gpu, target, &run)
            run.compressed = comptags(facade, target)
        case "shader_write":
            let target = try await made(facade, plain: plain) {
                try gpu.texture(.bgra8Unorm, 128, 128, usage: [.renderTarget, .shaderRead, .shaderWrite])
            }
            run.made = comptags(facade, target)
            let kernel = try gpu.compute("comp_pattern_kernel")
            run.paths.append(
                try await gpu.run { commands in
                    let p = MTLRenderPassDescriptor()
                    p.colorAttachments[0].texture = target
                    p.colorAttachments[0].loadAction = .clear
                    p.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                    p.colorAttachments[0].storeAction = .store
                    commands.makeRenderCommandEncoder(descriptor: p)!.endEncoding()
                    let e = commands.makeComputeCommandEncoder()!
                    e.setComputePipelineState(kernel)
                    e.setTexture(target, index: 0)
                    e.dispatchThreads(
                        MTLSize(width: 64, height: 128, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
                    e.endEncoding()
                }.path)
            run.bytes = try await gpu.read(target, bytesPerPixel: 4)
            run.compressed = comptags(facade, target)
        case "view_id":
            // a view and a gpuResourceID taken while compressed keep it compressed; after the move an m2v twin takes,
            // work through the view lands in the plain memory and the ID is the same
            let target = try await made(facade, plain: plain) { try gpu.texture(.bgra8Unorm, 128, 128) }
            run.made = comptags(facade, target)
            guard let view = target.makeTextureView(pixelFormat: .bgra8Unorm) else {
                throw TestFailure("no view made")
            }
            let id = target.gpuResourceID._impl
            run.compressed = comptags(facade, target)
            run.paths.append(try await gpu.run(draw(view, half: true)).path)
            run.bytes += try await gpu.read(target, bytesPerPixel: 4)
            if !plain {
                let moved = facade.perform(Self.moveSelector, with: target)?.takeUnretainedValue() as? NSNumber
                run.musts.append(("moved", moved?.boolValue == true ? 1 : 0))
                run.musts.append(("id_kept", target.gpuResourceID._impl == id ? 1 : 0))
            }
            run.paths.append(try await gpu.run(draw(view, half: true, clear: 1)).path)
            run.bytes += try await gpu.read(target, bytesPerPixel: 4)
            run.bytes += try await gpu.read(view, bytesPerPixel: 4)
        default:
            throw TestFailure("no shape arm \(arm)")
        }
        return run
    }
}
