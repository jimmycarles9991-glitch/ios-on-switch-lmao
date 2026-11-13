import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// What a Metal object made by the GM20B facade answers about itself in debug runtimes
/// (drivers/TegraGM20BMetal/raw/runtime/RawMetalObjectProbe.mm): its class and whether an m2v twin of it was ever made.
/// An object m2v made answers nothing.
enum NativeObject {
    static func answer(_ object: AnyObject) -> (name: String, twin: Bool)? {
        let selector = NSSelectorFromString("switchRawObject")
        guard let object = object as? NSObject, object.responds(to: selector),
            let answer = object.perform(selector)?.takeUnretainedValue() as? [String: Any]
        else { return nil }
        return (answer["class"] as? String ?? "?", (answer["twin"] as? NSNumber)?.boolValue ?? true)
    }
}

/// A workload's verdict: what went wrong, the objects it made through the device under test, and the paths of the
/// command buffers that used them.
struct NativeWork {
    var failures: [String] = []
    var objects: [AnyObject] = []
    var paths: [String] = []
}

/// A Metal object the raw facade makes itself: on the facade, the workload gives the right bytes, every object it made
/// is the device's own with no m2v twin made, and the command buffers that used them stayed raw.
protocol NativeObjectTest: HWTest {
    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork
    /// Why this build's identity has nothing of the feature to test (nil: it has)
    var unavailable: String? { get }
}

extension NativeObjectTest {
    var subsystem: Subsystem { .metal }
    var unavailable: String? { nil }

    func run(_ context: TestContext) async throws -> Outcome {
        if let why = unavailable { return .skip(why) }
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        guard !RawFacade.counters(of: gpu.device).isEmpty else {
            return .skip("a release runtime: objects do not answer switchRawObject")
        }
        var metrics = Metrics()
        var work = try await exercise(gpu, &metrics)
        var names: [String] = []
        for object in work.objects {
            guard let answer = NativeObject.answer(object) else {
                let name = NSStringFromClass(type(of: object))
                names.append(name)
                work.failures.append("\(name) is served by m2v")
                continue
            }
            names.append(answer.name)
            if answer.twin { work.failures.append("\(answer.name) made its m2v twin") }
        }
        metrics.set("objects", names.isEmpty ? "-" : Set(names).sorted().joined(separator: ","))
        metrics.set("paths", work.paths.isEmpty ? "-" : work.paths.joined(separator: ","))
        if let path = work.paths.first(where: { $0 != "raw" }) { work.failures.append("a command buffer went \(path)") }
        return work.failures.isEmpty
            ? .pass(metrics, proof: gpu.proof)
            : .fail(work.failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}

/// Depth/stencil states: the front face's write mask and the back face's own operation (one half of the target drawn
/// clockwise, the other counter-clockwise), a masked equal test whose reference changes between two draws of the same
/// state (only the first may pass), and a depth compare.
struct NativeDepthStencilTest: NativeObjectTest {
    let id = "metal.native.depth_stencil"
    let name = "Depth/stencil states are the device's own: both faces, masks, compares, per-draw references"

    static func face(
        _ compare: MTLCompareFunction, pass: MTLStencilOperation, read: UInt32 = 0xff, write: UInt32 = 0xff
    ) -> MTLStencilDescriptor {
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = compare
        face.depthStencilPassOperation = pass
        face.readMask = read
        face.writeMask = write
        return face
    }

    static func state(
        _ device: MTLDevice, depth: MTLCompareFunction, write: Bool, front: MTLStencilDescriptor? = nil,
        back: MTLStencilDescriptor? = nil
    ) throws -> MTLDepthStencilState {
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.depthCompareFunction = depth
        descriptor.isDepthWriteEnabled = write
        descriptor.frontFaceStencil = front
        descriptor.backFaceStencil = back
        guard let state = device.makeDepthStencilState(descriptor: descriptor) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        return state
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let size = 64
        let color = try gpu.texture(.rgba8Unorm, size, size)
        let depth = try gpu.texture(.depth32Float_stencil8, size, size, usage: [.renderTarget])
        let pipeline = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float_stencil8,
            stencil: .depth32Float_stencil8)
        let faces = try Self.state(
            gpu.device, depth: .less, write: true, front: Self.face(.always, pass: .replace, write: 0x3c),
            back: Self.face(.always, pass: .incrementClamp))
        let masked = Self.face(.equal, pass: .keep, read: 0x0c)
        let equal = try Self.state(gpu.device, depth: .always, write: false, front: masked, back: masked)
        let greater = try Self.state(gpu.device, depth: .greater, write: false)
        var work = NativeWork(objects: [faces, equal, greater])
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = color
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .store
            pass.stencilAttachment.texture = depth
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.clearStencil = 0
            pass.stencilAttachment.storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setDepthStencilState(faces)
            encoder.setStencilReferenceValues(front: 0xff, back: 0)
            encoder.setFrontFacing(.clockwise)
            GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: -1, x1: 0, y1: 1, z: 0.5), color: SIMD4(1, 0, 0, 1))
            encoder.setFrontFacing(.counterClockwise)
            GPU.draw(encoder, corners: GPU.quad(x0: 0, y0: -1, x1: 1, y1: 1, z: 0.5), color: SIMD4(1, 0, 0, 1))
            encoder.setDepthStencilState(equal)
            encoder.setStencilReferenceValue(0x0c)
            GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.1), color: SIMD4(0, 1, 0, 1))
            encoder.setStencilReferenceValue(0x04)
            GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.1), color: SIMD4(0, 0, 1, 1))
            encoder.setDepthStencilState(greater)
            GPU.draw(encoder, corners: GPU.quad(x0: -1, y0: 0, x1: 1, y1: 1, z: 0.9), color: SIMD4(1, 1, 0, 1))
            encoder.endEncoding()
        }
        work.paths.append(timing.path)
        let pixels = try await gpu.read(color, bytesPerPixel: 4)
        let stencils = try await gpu.read(depth, bytesPerPixel: 1, options: .stencilFromDepthStencil)
        // which half the clockwise quad's face is depends on the quad's own winding: one half holds the front face's
        // masked replace (0xff & 0x3c), the other the back face's increment
        let left = stencils[(size / 2) * size], right = stencils[(size / 2) * size + size - 1]
        metrics.set("stencil_halves", "\(left),\(right)")
        if Set([left, right]) != [0x3c, 1] { work.failures.append("stencil halves \(left)/\(right), not 0x3c and 1") }
        var wrongColor = 0, wrongStencil = 0
        for y in 0..<size {
            for x in 0..<size {
                let half = x < size / 2 ? left : right
                if stencils[y * size + x] != half { wrongStencil += 1 }
                let want: [UInt8] =
                    y < size / 2 ? [255, 255, 0, 255] : half == 0x3c ? [0, 255, 0, 255] : [255, 0, 0, 255]
                if Array(pixels.pixel(x, y, width: size)) != want { wrongColor += 1 }
            }
        }
        metrics.set("wrong_color", wrongColor)
        metrics.set("wrong_stencil", wrongStencil)
        if wrongColor != 0 { work.failures.append("\(wrongColor) pixels miss their test's colour") }
        if wrongStencil != 0 { work.failures.append("\(wrongStencil) stencil values differ from their half") }
        return work
    }
}

/// Fences: a blit's copy into a private buffer updates a fence that a render encoder of the same command buffer waits
/// for before its fragments read the buffer, and a second command buffer's pass waits for a fence the first one's blit
/// updated. The colours are the copied ones.
struct NativeFenceTest: NativeObjectTest {
    let id = "metal.native.fence"
    let name = "Fences are the device's own and order a blit's writes before a later pass's reads"

    static func copy(_ commands: MTLCommandBuffer, _ source: MTLBuffer, _ target: MTLBuffer, _ fence: MTLFence) {
        let blit = commands.makeBlitCommandEncoder()!
        blit.copy(from: source, sourceOffset: 0, to: target, destinationOffset: 0, size: 16)
        blit.updateFence(fence)
        blit.endEncoding()
    }

    static func draw(
        _ commands: MTLCommandBuffer, _ pipeline: MTLRenderPipelineState, into texture: MTLTexture,
        colour: MTLBuffer, after fence: MTLFence
    ) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.waitForFence(fence, before: .fragment)
        encoder.setRenderPipelineState(pipeline)
        var corners = GPU.fullScreen
        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
        encoder.setFragmentBuffer(colour, offset: 0, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let size = 32
        guard let inside = gpu.device.makeFence(), let across = gpu.device.makeFence(),
            let colour = gpu.device.makeBuffer(length: 16, options: .storageModePrivate)
        else { throw TestFailure("no fence or private buffer") }
        var work = NativeWork(objects: [inside, across])
        let magenta = try gpu.buffer([SIMD4<Float>(1, 0, 1, 1)]), green = try gpu.buffer([SIMD4<Float>(0, 1, 0, 1)])
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        let first = try gpu.texture(.rgba8Unorm, size, size), second = try gpu.texture(.rgba8Unorm, size, size)
        let one = try await gpu.run { commands in
            Self.copy(commands, magenta, colour, inside)
            Self.draw(commands, pipeline, into: first, colour: colour, after: inside)
        }
        let copied = try gpu.commit { Self.copy($0, green, colour, across) }
        let drawn = try gpu.commit { Self.draw($0, pipeline, into: second, colour: colour, after: across) }
        try gpu.finish([copied, drawn])
        work.paths += [one.path, RawFacade.path(of: copied), RawFacade.path(of: drawn)]
        var wrong = 0
        for (texture, want) in [(first, [UInt8](arrayLiteral: 255, 0, 255, 255)), (second, [0, 255, 0, 255])] {
            let pixels = try await gpu.read(texture, bytesPerPixel: 4)
            for y in 0..<size {
                for x in 0..<size where Array(pixels.pixel(x, y, width: size)) != want { wrong += 1 }
            }
        }
        metrics.set("wrong_pixels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) pixels miss the colour the fence ordered") }
        return work
    }
}

enum NativeObjectTests {
    static let all: [any HWTest] = [
        NativeDepthStencilTest(), NativeSharedEventTest(), NativeFenceTest(), NativeArgumentEncoderTest(),
        NativeBinaryArchiveTest(), NativeHiddenDepthTest(), NativeEventWaitTest(),
    ]
}
