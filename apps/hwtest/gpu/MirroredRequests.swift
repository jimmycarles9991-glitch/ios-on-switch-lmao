import Foundation
import HWTestCore
import Metal

/// Requests Metal's validation layer stops (the scenes of gpu.gm20b.invalid_requests's later arms): a copy between
/// formats of other copy units, which the driver records nothing for, and, each as an M4 Max's driver runs it with
/// validation off, indexed draws of an indirect command buffer at an index offset off their type, a counter resolve
/// into an offset off 4 bytes, an execute whose range is read at an offset off 4 bytes and one read past its buffer,
/// and a layered pass past one attachment's layers beside a format stand-in.
public enum MirroredRequests {
    public static let side = 64, region = 16, bytes = 4096

    public struct Copied: Sendable {
        public var path = "-"
        /// Bytes of the two neighbouring buffer copies off their pattern
        public var neighbours = -1
        /// The destination's texels after the copy, RGBA8 rows of `side`
        public var texels: [UInt8] = []
    }

    /// RGBA16Float 64x64 (texel (x, y) = halves x/64, y/64, 0.5, 1) copied from (4, 2), 16x16, to (0, 0) of an
    /// RGBA8Unorm 64x64 of 0x22 bytes, between two buffer copies of a pattern in the same blit encoder
    public static func units(_ gpu: GPU) async throws -> Copied {
        let source = try gpu.texture(.rgba16Float, side, side, usage: [.shaderRead])
        var halves: [Float16] = []
        for y in 0 ..< side {
            for x in 0 ..< side { halves += [Float16(Float(x) / 64), Float16(Float(y) / 64), 0.5, 1] }
        }
        try await gpu.upload(halves.withUnsafeBytes { [UInt8]($0) }, to: source, bytesPerRow: side * 8)
        let destination = try gpu.texture(.rgba8Unorm, side, side, usage: [.shaderRead])
        try await gpu.upload([UInt8](repeating: 0x22, count: side * side * 4), to: destination, bytesPerRow: side * 4)
        let pattern = try gpu.buffer((0 ..< bytes).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 3) })
        let first = try gpu.buffer(bytes), last = try gpu.buffer(bytes)
        var out = Copied()
        out.path = try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(from: pattern, sourceOffset: 0, to: first, destinationOffset: 0, size: bytes)
            blit.copy(
                from: source, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 4, y: 2, z: 0),
                sourceSize: MTLSize(width: region, height: region, depth: 1), to: destination, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
            blit.copy(from: pattern, sourceOffset: 0, to: last, destinationOffset: 0, size: bytes)
            blit.endEncoding()
        }.facadePath
        out.neighbours = differing(first, pattern) + differing(last, pattern)
        out.texels = try await gpu.read(destination, bytesPerPixel: 4)
        return out
    }

    static func differing(_ a: MTLBuffer, _ b: MTLBuffer) -> Int {
        let x = UnsafeRawBufferPointer(start: a.contents(), count: bytes)
        let y = UnsafeRawBufferPointer(start: b.contents(), count: bytes)
        return zip(x, y).filter { $0 != $1 }.count
    }

    public struct Drawn: Sendable {
        public var path = "-"
        /// The 48x48 RGBA8 target's red bytes, one a pixel
        public var red: [UInt8] = []
        public var redPixels: Int { red.filter { $0 == 255 }.count }
    }

    public static let gridSide = 48

    /// One indexed draw of an indirect command buffer (DrawIndexed, inheriting the pipeline and buffers) of the indices
    /// {0, 1, 4, 5, 15, 15} of `type` at `offset` into a 48x48 RGBA8 target cleared black, the grid's points as its
    /// vertices: the triangle it covers names the indices it read
    public static func indexed(_ gpu: GPU, _ type: MTLIndexType, offset: Int) async throws -> Drawn {
        let descriptor = MTLIndirectCommandBufferDescriptor()
        descriptor.commandTypes = [.drawIndexed]
        descriptor.inheritPipelineState = true
        descriptor.inheritBuffers = true
        guard let icb = gpu.device.makeIndirectCommandBuffer(descriptor: descriptor, maxCommandCount: 1, options: [])
        else { throw TestFailure("no indirect command buffer of indexed draws") }
        let words: [UInt32] = [0, 1, 4, 5, 15, 15]
        let indices = try type == .uint32 ? gpu.buffer(words + [UInt32](repeating: 0, count: 10))
            : gpu.buffer(words.map(UInt16.init) + [UInt16](repeating: 0, count: 26))
        icb.indirectRenderCommandAt(0).drawIndexedPrimitives(
            .triangle, indexCount: 3, indexType: type, indexBuffer: indices, indexBufferOffset: offset,
            instanceCount: 1, baseVertex: 0, baseInstance: 0
        )
        let state = try gpu.render(
            vertex: "mirrored_grid_vertex", fragment: "mirrored_red_fragment", format: .rgba8Unorm
        ) { $0.supportIndirectCommandBuffers = true }
        let target = try gpu.texture(.rgba8Unorm, gridSide, gridSide)
        var out = Drawn()
        out.path = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            encoder.useResource(indices, usage: .read, stages: .vertex)
            encoder.executeCommandsInBuffer(icb, range: 0 ..< 1)
            encoder.endEncoding()
        }.facadePath
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        out.red = stride(from: 0, to: pixels.count, by: 4).map { pixels[$0] }
        return out
    }

    public struct Resolved: Sendable {
        public var paths: [String] = []
        /// Each destination's 4096 bytes after resolving the samples at offset 0 and at offset 2
        public var atZero: [UInt8] = [], atTwo: [UInt8] = []
    }

    /// Four timestamp samples of a blit encoder's start and end (samples 2 and 3 never written), resolved by a blit at
    /// offset 0 of one buffer of 0x22 bytes and at offset 2 of another, each in a command buffer of its own
    public static func resolveOff(_ gpu: GPU) async throws -> Resolved {
        guard let set = gpu.device.counterSets?.first(where: { $0.name == MTLCommonCounterSet.timestamp.rawValue })
        else { throw TestFailure("no timestamp counter set") }
        let descriptor = MTLCounterSampleBufferDescriptor()
        descriptor.counterSet = set
        descriptor.sampleCount = 4
        descriptor.storageMode = .shared
        let samples = try gpu.device.makeCounterSampleBuffer(descriptor: descriptor)
        let pass = MTLBlitPassDescriptor()
        pass.sampleBufferAttachments[0].sampleBuffer = samples
        pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = 0
        pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = 1
        let filled = try gpu.buffer(64)
        var out = Resolved()
        out.paths.append(
            try await gpu.run(tolerateError: true) { commands in
                let blit = commands.makeBlitCommandEncoder(descriptor: pass)!
                blit.fill(buffer: filled, range: 0 ..< 64, value: 1)
                blit.endEncoding()
            }.facadePath
        )
        let atZero = try gpu.buffer([UInt8](repeating: 0x22, count: bytes))
        let atTwo = try gpu.buffer([UInt8](repeating: 0x22, count: bytes))
        for (destination, offset) in [(atZero, 0), (atTwo, 2)] {
            out.paths.append(
                try await gpu.run(tolerateError: true) { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.resolveCounters(
                        samples, range: 0 ..< 4, destinationBuffer: destination, destinationOffset: offset
                    )
                    blit.endEncoding()
                }.facadePath
            )
        }
        out.atZero = [UInt8](UnsafeRawBufferPointer(start: atZero.contents(), count: bytes))
        out.atTwo = [UInt8](UnsafeRawBufferPointer(start: atTwo.contents(), count: bytes))
        return out
    }

    public struct Executed: Sendable {
        public var status = -1
        /// "none", or the error's "<domain>/<code>"
        public var error = "-", path = "-"
        /// Bytes the blit encoders before and after the execute wrote of their 4096
        public var beforeWritten = -1, afterWritten = -1
        /// Bit i set where command i drew: its triangle lies in quadrant i of the 16x16 target
        public var drawn = -1
    }

    /// range_off: the range {0, 4} at byte 0 of a 4096-byte buffer of zeros, read at offset 2, which an M4 Max reads
    /// from the offset rounded down to 4 bytes
    public static func rangeOff(_ gpu: GPU) async throws -> Executed {
        let range = try gpu.buffer([UInt8](repeating: 0, count: bytes))
        let words: [UInt32] = [0, 4]
        words.withUnsafeBytes { range.contents().copyMemory(from: $0.baseAddress!, byteCount: 8) }
        return try await executed(gpu, range, offset: 2, heap: nil)
    }

    public static let heapBytes = 65536

    /// range_past: a 4096-byte buffer of zeros read at offset 4092, 4 bytes of the range inside it. It lies at the
    /// start of a placement heap that a zeroed buffer spans whole, so what an M4 Max reads past it is zeros: a range
    /// that runs no command.
    public static func rangePast(_ gpu: GPU) async throws -> Executed {
        let descriptor = MTLHeapDescriptor()
        descriptor.type = .placement
        descriptor.storageMode = .shared
        descriptor.size = gpu.device.heapBufferSizeAndAlign(length: heapBytes, options: .storageModeShared).size
        guard let heap = gpu.device.makeHeap(descriptor: descriptor),
              let whole = heap.makeBuffer(length: heapBytes, options: .storageModeShared, offset: 0)
        else { throw TestFailure("no placement heap of \(heapBytes) bytes") }
        memset(whole.contents(), 0, heapBytes)
        guard let range = heap.makeBuffer(length: bytes, options: .storageModeShared, offset: 0) else {
            throw TestFailure("no range buffer at the heap's start")
        }
        memset(range.contents(), 0, bytes)
        return try await executed(gpu, range, offset: bytes - 4, heap: heap)
    }

    /// A command buffer of a blit filling 4096 bytes, a render pass executing four draws of an indirect command buffer
    /// (draw i one triangle in quadrant i) whose range it reads at `offset` of `range`, and a blit filling another 4096
    /// bytes
    static func executed(_ gpu: GPU, _ range: MTLBuffer, offset: Int, heap: MTLHeap?) async throws -> Executed {
        let descriptor = MTLIndirectCommandBufferDescriptor()
        descriptor.commandTypes = [.draw]
        descriptor.inheritPipelineState = true
        descriptor.inheritBuffers = true
        guard let icb = gpu.device.makeIndirectCommandBuffer(descriptor: descriptor, maxCommandCount: 4, options: [])
        else { throw TestFailure("no indirect command buffer of draws") }
        for i in 0 ..< 4 {
            icb.indirectRenderCommandAt(i).drawPrimitives(
                .triangle, vertexStart: 3 * i, vertexCount: 3, instanceCount: 1, baseInstance: 0
            )
        }
        let state = try gpu.render(
            vertex: "mirrored_quadrant_vertex", fragment: "mirrored_red_fragment", format: .rgba8Unorm
        ) { $0.supportIndirectCommandBuffers = true }
        let target = try gpu.texture(.rgba8Unorm, 16, 16)
        let before = try gpu.buffer([UInt8](repeating: 0x11, count: bytes))
        let after = try gpu.buffer([UInt8](repeating: 0x11, count: bytes))
        guard let commands = gpu.queue.makeCommandBuffer() else { throw TestFailure("no command buffer") }
        let first = commands.makeBlitCommandEncoder()!
        first.fill(buffer: before, range: 0 ..< bytes, value: 0x5a)
        first.endEncoding()
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[0].storeAction = .store
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        if let heap { encoder.useHeap(heap, stages: [.vertex, .fragment]) }
        encoder.setRenderPipelineState(state)
        encoder.executeCommandsInBuffer(icb, indirectBuffer: range, offset: offset)
        encoder.endEncoding()
        let last = commands.makeBlitCommandEncoder()!
        last.fill(buffer: after, range: 0 ..< bytes, value: 0x5a)
        last.endEncoding()
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
            commands.addCompletedHandler { _ in done.resume() }
            commands.commit()
        }
        var out = Executed()
        out.status = Int(commands.status.rawValue)
        out.path = GM20BFacade.path(of: commands)
        out.error = (commands.error as NSError?).map { "\($0.domain)/\($0.code)" } ?? "none"
        let written = { (b: MTLBuffer) in
            UnsafeRawBufferPointer(start: b.contents(), count: bytes).filter { $0 != 0x11 }.count
        }
        out.beforeWritten = written(before)
        out.afterWritten = written(after)
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        out.drawn = 0
        for at in stride(from: 0, to: pixels.count, by: 4) where pixels[at] == 255 && pixels[at + 1] == 0 {
            let x = at / 4 % 16, y = at / 64
            out.drawn |= 1 << ((x / 8) + 2 * (y / 8))
        }
        withExtendedLifetime(heap) {}
        return out
    }

    public struct Layered: Sendable {
        public var paths: [String] = []
        /// Per attachment, per layer: whether it holds its clear everywhere (1), its fill everywhere (0) or neither
        /// (-1)
        public var cleared: [[Int]] = []
    }

    public static let layerSide = 16

    /// Clear-only passes of renderTargetArrayLength 3, run `runs` times, into 16x16 2D arrays, every layer of each
    /// filled with 0x11 bytes first: `formats` with their layer counts as colours 0, 1, ..., each attached from `slice`
    public static func layered(
        _ gpu: GPU, _ formats: [(MTLPixelFormat, layers: Int, bytesPerPixel: Int)], slice: Int, runs: Int
    ) async throws -> Layered {
        var arrays: [MTLTexture] = []
        for (format, layers, bytesPerPixel) in formats {
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: format, width: layerSide, height: layerSide, mipmapped: false
            )
            d.textureType = .type2DArray
            d.arrayLength = layers
            d.usage = [.renderTarget, .shaderRead]
            d.storageMode = .private
            guard let t = gpu.device.makeTexture(descriptor: d) else {
                throw TestFailure("no \(format.rawValue) array")
            }
            let fill = [UInt8](repeating: 0x11, count: layerSide * layerSide * bytesPerPixel)
            for layer in 0 ..< layers {
                try await gpu.upload(fill, to: t, bytesPerRow: layerSide * bytesPerPixel, slice: layer)
            }
            arrays.append(t)
        }
        var out = Layered()
        for _ in 0 ..< runs {
            out.paths.append(
                try await gpu.run(tolerateError: true) { commands in
                    let pass = MTLRenderPassDescriptor()
                    for (k, t) in arrays.enumerated() {
                        pass.colorAttachments[k].texture = t
                        pass.colorAttachments[k].slice = slice
                        pass.colorAttachments[k].loadAction = .clear
                        pass.colorAttachments[k].clearColor = MTLClearColor(red: 1, green: 0, blue: 1, alpha: 1)
                        pass.colorAttachments[k].storeAction = .store
                    }
                    pass.renderTargetArrayLength = 3
                    commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
                }.facadePath
            )
        }
        for (t, (_, layers, bytesPerPixel)) in zip(arrays, formats) {
            var states: [Int] = []
            for layer in 0 ..< layers {
                let texels = try await gpu.read(t, bytesPerPixel: bytesPerPixel, slice: layer)
                let first = Array(texels[0 ..< bytesPerPixel])
                let uniform = stride(from: 0, to: texels.count, by: bytesPerPixel).allSatisfy {
                    Array(texels[$0 ..< ($0 + bytesPerPixel)]) == first
                }
                states.append(!uniform ? -1 : first.allSatisfy { $0 == 0x11 } ? 0 : 1)
            }
            out.cleared.append(states)
        }
        return out
    }
}
