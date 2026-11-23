import Foundation
import HWTestCore
import HWTestGPU
import Metal

extension StorageFormsCases {
    final class Scene {
        let gpu: GPU
        let library: MTLLibrary
        private var states: [String: MTLRenderPipelineState] = [:]

        init(_ gpu: GPU) async throws {
            self.gpu = gpu
            let options = MTLCompileOptions()
            if #available(iOS 18.0, macOS 15.0, *) {
                options.mathMode = .safe
            } else {
                options.fastMathEnabled = false
            }
            library = try await gpu.device.makeLibrary(source: StorageFormsCases.source, options: options)
        }

        func state(
            _ key: String, _ describe: (MTLRenderPipelineDescriptor) -> Void
        ) async throws -> MTLRenderPipelineState {
            if let made = states[key] { return made }
            let d = MTLRenderPipelineDescriptor()
            describe(d)
            let made = try await gpu.device.makeRenderPipelineState(descriptor: d)
            states[key] = made
            return made
        }

        func state(_ c: Case) async throws -> MTLRenderPipelineState {
            let depth: MTLPixelFormat = c.tests == nil ? .invalid : .depth32Float_stencil8
            return try await state("\(c.fragment)/\(c.format.rawValue)/\(c.samples)/\(depth.rawValue)") { d in
                d.vertexFunction = library.makeFunction(name: "sf_triangles")
                d.fragmentFunction = library.makeFunction(name: c.fragment)
                d.colorAttachments[0].pixelFormat = c.format
                d.rasterSampleCount = c.samples
                d.depthAttachmentPixelFormat = depth
                d.stencilAttachmentPixelFormat = depth
            }
        }

        func screen(_ fragment: String, _ formats: [MTLPixelFormat]) async throws -> MTLRenderPipelineState {
            try await state(fragment) { d in
                d.vertexFunction = library.makeFunction(name: "sf_screen")
                d.fragmentFunction = library.makeFunction(name: fragment)
                for (index, format) in formats.enumerated() { d.colorAttachments[index].pixelFormat = format }
            }
        }

        func depthStencil(_ tests: Tests) throws -> MTLDepthStencilState {
            let d = MTLDepthStencilDescriptor()
            d.depthCompareFunction = tests == .pass ? .always : .lessEqual
            d.isDepthWriteEnabled = tests == .depthwrite || tests == .stencilwrite
            if tests == .stencilwrite {
                let stencil = MTLStencilDescriptor()
                stencil.stencilCompareFunction = .always
                stencil.depthStencilPassOperation = .incrementClamp
                d.frontFaceStencil = stencil
                d.backFaceStencil = stencil
            }
            guard let made = gpu.device.makeDepthStencilState(descriptor: d) else {
                throw TestFailure("no depth stencil state")
            }
            return made
        }

        struct Targets {
            let colour: MTLTexture
            let depth: MTLTexture?
            let tests: MTLDepthStencilState?
        }

        func targets(_ c: Case, width: Int, height: Int) throws -> Targets {
            Targets(
                colour: try gpu.texture(c.format, width, height, samples: c.samples),
                depth: try c.tests.map { _ in
                    try gpu.texture(.depth32Float_stencil8, width, height, usage: [.renderTarget], samples: c.samples)
                },
                tests: try c.tests.map(depthStencil)
            )
        }

        func encode(
            _ c: Case, _ state: MTLRenderPipelineState, _ targets: Targets, zStep: Float, _ commands: MTLCommandBuffer
        ) {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = targets.colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = StorageFormsCases.clear
            pass.colorAttachments[0].storeAction = .store
            if let depth = targets.depth {
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 1
                pass.depthAttachment.storeAction = .dontCare
                pass.stencilAttachment.texture = depth
                pass.stencilAttachment.loadAction = .clear
                pass.stencilAttachment.storeAction = .dontCare
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            if let tests = targets.tests { encoder.setDepthStencilState(tests) }
            var params = StorageFormsCases.steps + [1 / 31, zStep]
            encoder.setVertexBytes(&params, length: params.count * 4, index: 0)
            encoder.setFragmentBytes(&params, length: params.count * 4, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3 * c.triangles)
            encoder.endEncoding()
        }

        func waited(_ encode: (MTLCommandBuffer) throws -> Void) throws -> MTLCommandBuffer {
            let buffer = try gpu.commit(encode)
            buffer.waitUntilCompleted()
            return buffer
        }

        static func failure(_ buffer: MTLCommandBuffer) -> NSError? {
            buffer.status == .error ? (buffer.error as NSError?) ?? NSError(domain: "no error", code: -1) : nil
        }

        func observe(_ c: Case) async throws -> Observation {
            let state = try await state(c)
            let targets = try targets(c, width: side, height: side)
            let buffer = try waited { encode(c, state, targets, zStep: 1.0 / 32, $0) }
            var seen = Observation(error: Self.failure(buffer), path: GM20BFacade.path(of: buffer))
            if seen.error == nil { seen.pixels = try await pixels(c, targets.colour) }
            return seen
        }

        func pixels(_ c: Case, _ colour: MTLTexture) async throws -> [[Int]] {
            let count = side * side
            if c.samples > 1 {
                let unpacked = try gpu.texture(.rgba32Float, side * 4, side)
                let unpack = try await screen("sf_unpack", [.rgba32Float])
                try await gpu.run { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = unpacked
                    pass.colorAttachments[0].loadAction = .dontCare
                    pass.colorAttachments[0].storeAction = .store
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(unpack)
                    encoder.setFragmentTexture(colour, index: 0)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                    encoder.endEncoding()
                }
                let lanes = try await gpu.read(unpacked, bytesPerPixel: 16).withUnsafeBytes {
                    Array($0.bindMemory(to: Float.self))
                }
                return (0 ..< count).map { i in
                    [0, 3].flatMap { s in (0 ..< 4).map { Int((lanes[(i * 4 + s) * 4 + $0] * 255).rounded()) } }
                }
            }
            if c.format == .bgr5A1Unorm {
                let bytes = try await gpu.read(colour, bytesPerPixel: 2)
                return (0 ..< count).map { [Int(bytes[2 * $0]) | Int(bytes[2 * $0 + 1]) << 8] }
            }
            let bytes = try await gpu.read(colour, bytesPerPixel: 4)
            return (0 ..< count).map { bytes[4 * $0 ..< 4 * $0 + 4].map(Int.init) }
        }

        func loaded(half: Bool) async throws -> Loaded {
            let texels = (0 ..< side * side).map(StorageFormsCases.loadedTexel)
            let colour = try gpu.texture(.bgr5A1Unorm, side, side)
            try await gpu.upload(
                texels.flatMap { [UInt8($0 & 0xff), UInt8($0 >> 8)] }, to: colour, bytesPerRow: side * 2
            )
            let copy = try gpu.texture(.rgba32Float, side, side)
            let state = try await screen(half ? "sf_load_half" : "sf_load_float", [.bgr5A1Unorm, .rgba32Float])
            let buffer = try waited { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = colour
                pass.colorAttachments[0].loadAction = .load
                pass.colorAttachments[0].storeAction = .store
                pass.colorAttachments[1].texture = copy
                pass.colorAttachments[1].loadAction = .clear
                pass.colorAttachments[1].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(state)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                encoder.endEncoding()
            }
            var seen = Loaded(error: Self.failure(buffer), path: GM20BFacade.path(of: buffer))
            guard seen.error == nil else { return seen }
            let after = try await gpu.read(colour, bytesPerPixel: 2)
            let words = try await gpu.read(copy, bytesPerPixel: 16).withUnsafeBytes {
                Array($0.bindMemory(to: UInt32.self))
            }
            for (i, texel) in texels.enumerated() {
                if UInt16(after[2 * i]) | UInt16(after[2 * i + 1]) << 8 != texel { seen.texelsChanged += 1 }
                let want = StorageFormsCases.loadedLanes(texel)
                seen.lanesOff += (0 ..< 4).filter { words[4 * i + $0] != want[$0] }.count
            }
            return seen
        }

        func microseconds(_ c: Case) async throws -> Double {
            let state = try await state(c)
            let targets = try targets(c, width: timedWidth, height: timedHeight)
            let zStep = 1 / Float(4 * timedTriangles)
            _ = try await gpu.run(tolerateError: true) { encode(c, state, targets, zStep: zStep, $0) }
            var best = Double.infinity
            for _ in 0 ..< timedRuns {
                let timing = try await gpu.run { encode(c, state, targets, zStep: zStep, $0) }
                guard timing.gpuTimed else { return 0 }
                best = min(best, timing.gpuMilliseconds * 1000)
            }
            return best
        }
    }
}
