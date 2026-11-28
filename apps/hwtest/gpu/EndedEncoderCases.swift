import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Commands recorded on an encoder after its endEncoding (gpu.gm20b.ended_encoder), each case one command buffer over a
/// 64x64 RGBA8 target T cleared to (0.25, 0.25, 0.25, 1) and 4 KiB shared buffers: src byte i = i (i < 256), src2 all
/// 0xaa, out and out2 zero. A "snap" is a blit after the late command in the same command buffer.
public final class EndedEncoderBench {
    public struct Reading: Sendable {
        /// completed or error, the error's MTLCommandBufferError code, the facade's path
        public var status = "completed", code = 0, path = "-"
        /// T's pixels at (16, 16), (48, 16), (16, 48), (48, 48) after the command buffer, and in its snap
        public var pixels: [String: [UInt8]] = [:], snap: [String: [UInt8]] = [:]
        public var out: [UInt8] = [], out2: [UInt8] = [], snapOut: [UInt8] = []
    }

    public static let corners = [("tl", 16, 16), ("tr", 48, 16), ("bl", 16, 48), ("br", 48, 48)]
    static let left = 0, right = 1, whole = 2, top = 3

    let gpu: GPU
    let flat, dark, colour: MTLRenderPipelineState
    let set, double, increment: MTLComputePipelineState
    let target: MTLTexture
    var src, src2, out, out2, snapTarget, snapOut: MTLBuffer

    public init(_ gpu: GPU) throws {
        self.gpu = gpu
        let state = { (fragment: String) in
            try gpu.render(vertex: "ended_quad", fragment: fragment, format: .rgba8Unorm)
        }
        (flat, dark, colour) = (try state("ended_flat"), try state("ended_dark"), try state("ended_buffer"))
        (set, double, increment) = (
            try gpu.compute("ended_set"),
            try gpu.compute("ended_double"),
            try gpu.compute("ended_increment")
        )
        target = try gpu.texture(.rgba8Unorm, 64, 64, usage: [.renderTarget, .shaderRead])
        (src, src2, out, out2) = (
            try gpu.buffer(4096),
            try gpu.buffer(4096),
            try gpu.buffer(4096),
            try gpu.buffer(4096)
        )
        (snapTarget, snapOut) = (try gpu.buffer(64 * 64 * 4), try gpu.buffer(4096))
    }

    func reset() throws {
        src = try gpu.buffer((0 ..< 4096).map { UInt8(truncatingIfNeeded: $0 < 256 ? $0 : 0) })
        src2 = try gpu.buffer([UInt8](repeating: 0xaa, count: 4096))
        (out, out2) = (try gpu.buffer(4096), try gpu.buffer(4096))
        (snapTarget, snapOut) = (try gpu.buffer(64 * 64 * 4), try gpu.buffer(4096))
    }

    func pass(_ commands: MTLCommandBuffer, load: Bool = false) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = load ? .load : .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.25, green: 0.25, blue: 0.25, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        return pass
    }

    func render(_ commands: MTLCommandBuffer, _ state: MTLRenderPipelineState,
                load: Bool = false) -> MTLRenderCommandEncoder {
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass(commands, load: load))!
        encoder.setRenderPipelineState(state)
        return encoder
    }

    static func draw(_ encoder: MTLRenderCommandEncoder, _ quad: Int) {
        encoder.drawPrimitives(type: .triangle, vertexStart: quad * 6, vertexCount: 6)
    }

    func compute(_ commands: MTLCommandBuffer, _ state: MTLComputePipelineState) -> MTLComputeCommandEncoder {
        let encoder = commands.makeComputeCommandEncoder()!
        encoder.setComputePipelineState(state)
        encoder.setBuffer(out, offset: 0, index: 0)
        return encoder
    }

    static func dispatch(_ encoder: MTLComputeCommandEncoder) {
        encoder.dispatchThreads(
            MTLSize(width: 64, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
        )
    }

    func snap(_ commands: MTLCommandBuffer) {
        let blit = commands.makeBlitCommandEncoder()!
        blit.copy(
            from: target, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
            sourceSize: MTLSize(width: 64, height: 64, depth: 1), to: snapTarget, destinationOffset: 0,
            destinationBytesPerRow: 256, destinationBytesPerImage: 0
        )
        blit.copy(from: out, sourceOffset: 0, to: snapOut, destinationOffset: 0, size: 4096)
        blit.endEncoding()
    }

    /// The command buffer's encode for `name`; nil for no such case
    func encode(_ name: String) -> ((MTLCommandBuffer) -> Void)? {
        let b = self
        switch name {
        case "late_draw", "two_late", "then_load":
            return { c in
                let e = b.render(c, b.flat)
                Self.draw(e, Self.whole)
                e.endEncoding()
                Self.draw(e, Self.left)
                if name == "two_late" { Self.draw(e, Self.top) }
                b.snap(c)
                if name == "then_load" {
                    let next = b.render(c, b.dark, load: true)
                    Self.draw(next, Self.top)
                    next.endEncoding()
                }
            }
        case "late_pipeline", "bind_only":
            return { c in
                let e = b.render(c, b.flat)
                Self.draw(e, Self.whole)
                e.endEncoding()
                e.setRenderPipelineState(b.dark)
                if name == "late_pipeline" { Self.draw(e, Self.left) }
                b.snap(c)
            }
        case "late_buffer", "late_bytes":
            return { c in
                var colours: [SIMD4<Float>] = [SIMD4(1, 0, 0, 1), SIMD4(0, 1, 0, 1)]
                let e = b.render(c, b.colour)
                if name == "late_buffer" {
                    let buffer = try? b.gpu.buffer(colours)
                    e.setFragmentBuffer(buffer, offset: 0, index: 0)
                    Self.draw(e, Self.whole)
                    e.endEncoding()
                    e.setFragmentBufferOffset(16, index: 0)
                } else {
                    e.setFragmentBytes(&colours[0], length: 16, index: 0)
                    Self.draw(e, Self.whole)
                    e.endEncoding()
                    e.setFragmentBytes(&colours[1], length: 16, index: 0)
                }
                Self.draw(e, Self.left)
            }
        case "parallel_sub", "parallel_next", "parallel_after":
            return { c in
                let parallel = c.makeParallelRenderCommandEncoder(descriptor: b.pass(c))!
                let first = parallel.makeRenderCommandEncoder()!
                first.setRenderPipelineState(b.flat)
                Self.draw(first, Self.whole)
                first.endEncoding()
                switch name {
                case "parallel_sub":
                    Self.draw(first, Self.left)
                    parallel.endEncoding()
                case "parallel_next":
                    let second = parallel.makeRenderCommandEncoder()!
                    second.setRenderPipelineState(b.dark)
                    Self.draw(first, Self.left)
                    Self.draw(second, Self.top)
                    second.endEncoding()
                    parallel.endEncoding()
                default:
                    parallel.endEncoding()
                    Self.draw(first, Self.left)
                }
            }
        case "blit_late_copy":
            return { c in
                let blit = c.makeBlitCommandEncoder()!
                blit.copy(from: b.src, sourceOffset: 0, to: b.out, destinationOffset: 0, size: 64)
                blit.endEncoding()
                blit.copy(from: b.src2, sourceOffset: 0, to: b.out, destinationOffset: 0, size: 32)
                blit.fill(buffer: b.out, range: 128 ..< 160, value: 0x55)
                b.snap(c)
            }
        case "blit_earlier":
            return { c in
                let first = c.makeBlitCommandEncoder()!
                first.copy(from: b.src, sourceOffset: 0, to: b.out, destinationOffset: 0, size: 64)
                first.endEncoding()
                let second = c.makeBlitCommandEncoder()!
                second.fill(buffer: b.out, range: 0 ..< 64, value: 0x11)
                second.endEncoding()
                first.copy(from: b.src2, sourceOffset: 0, to: b.out, destinationOffset: 0, size: 32)
            }
        case "late_dispatch", "late_kernel", "late_kernel_buf":
            return { c in
                let e = b.compute(c, b.set)
                Self.dispatch(e)
                if name == "late_dispatch" {
                    e.setComputePipelineState(b.double)
                    Self.dispatch(e)
                }
                e.endEncoding()
                switch name {
                case "late_kernel": e.setComputePipelineState(b.increment)
                case "late_kernel_buf": e.setBuffer(b.out2, offset: 0, index: 0)
                default: break
                }
                Self.dispatch(e)
                if name == "late_dispatch" { b.snap(c) }
            }
        case "draw_in_blit":
            return { c in
                let e = b.render(c, b.flat)
                Self.draw(e, Self.whole)
                e.endEncoding()
                let blit = c.makeBlitCommandEncoder()!
                blit.fill(buffer: b.out2, range: 0 ..< 64, value: 0x22)
                Self.draw(e, Self.left)
                blit.endEncoding()
            }
        case "copy_after_pass", "copy_after_event":
            return { c in
                let blit = c.makeBlitCommandEncoder()!
                blit.fill(buffer: b.out, range: 0 ..< 64, value: 0x33)
                blit.endEncoding()
                if name == "copy_after_pass" {
                    b.render(c, b.flat).endEncoding()
                } else if let event = b.gpu.device.makeEvent() {
                    c.encodeSignalEvent(event, value: 1)
                }
                blit.copy(from: b.src, sourceOffset: 0, to: b.out, destinationOffset: 0, size: 64)
            }
        case "end_twice_render":
            return { c in
                let e = b.render(c, b.flat)
                Self.draw(e, Self.whole)
                e.endEncoding()
                e.endEncoding()
            }
        case "end_twice_blit":
            return { c in
                let blit = c.makeBlitCommandEncoder()!
                blit.fill(buffer: b.out, range: 0 ..< 64, value: 0x44)
                blit.endEncoding()
                blit.endEncoding()
            }
        case "end_twice_compute":
            return { c in
                let e = b.compute(c, b.set)
                Self.dispatch(e)
                e.endEncoding()
                e.endEncoding()
            }
        case "begin_while_open":
            return { c in
                let first = c.makeBlitCommandEncoder()!
                first.fill(buffer: b.out, range: 0 ..< 64, value: 0x55)
                let second = c.makeBlitCommandEncoder()!
                second.fill(buffer: b.out2, range: 0 ..< 64, value: 0x66)
                first.endEncoding()
                second.endEncoding()
            }
        default:
            return nil
        }
    }

    public func run(_ name: String) async throws -> Reading {
        guard let encode = encode(name) else { throw TestFailure("no case \(name)") }
        try reset()
        let timing = try await gpu.run(tolerateError: true, encode)
        var reading = Reading(path: timing.facadePath)
        if timing.path.hasPrefix("error:") {
            reading.status = "error"
            reading.code = timing.path.range(of: #"/\d+"#, options: .regularExpression)
                .map { Int(timing.path[$0].dropFirst()) ?? -1 } ?? -1
        }
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        let snapped = [UInt8](UnsafeRawBufferPointer(start: snapTarget.contents(), count: 64 * 64 * 4))
        for (key, x, y) in Self.corners {
            reading.pixels[key] = Array(bytes[(y * 64 + x) * 4 ..< (y * 64 + x) * 4 + 4])
            reading.snap[key] = Array(snapped[(y * 64 + x) * 4 ..< (y * 64 + x) * 4 + 4])
        }
        let copy = { (buffer: MTLBuffer) in [UInt8](UnsafeRawBufferPointer(start: buffer.contents(), count: 512)) }
        (reading.out, reading.out2, reading.snapOut) = (copy(out), copy(out2), copy(snapOut))
        return reading
    }
}
