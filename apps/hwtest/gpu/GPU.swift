import Foundation
import HWTestCore
import Metal

/// What every Metal test needs: the device, its library, pipelines, command buffers awaited with their GPU timestamps
/// and the path the raw facade sent each down, and readback of private (tiled) textures through the blit engine, the
/// way apps do it.
public final class GPU: @unchecked Sendable {
    public let device: MTLDevice
    public let queue: MTLCommandQueue
    public let library: MTLLibrary
    private let lock = NSLock()
    private var recorded = PathTally()
    private var failures: [String] = []
    private var hostClocked = 0

    /// The app's precompiled default.metallib; the host tests compile the shaders' sources instead.
    nonisolated(unsafe) public static var makeLibrary: (MTLDevice) throws -> MTLLibrary = {
        try $0.makeDefaultLibrary(bundle: .main)
    }

    public static func make(device: MTLDevice? = nil) throws -> GPU {
        guard let device = device ?? MTLCreateSystemDefaultDevice() else {
            throw TestFailure("MTLCreateSystemDefaultDevice returned nil")
        }
        guard let queue = device.makeCommandQueue() else { throw TestFailure("makeCommandQueue returned nil") }
        return GPU(device: device, queue: queue, library: try makeLibrary(device))
    }

    public init(device: MTLDevice, queue: MTLCommandQueue, library: MTLLibrary) {
        self.device = device
        self.queue = queue
        self.library = library
    }

    public struct Timing: Sendable {
        public let gpuMilliseconds: Double
        /// From the commit to the completed handler, on the host's clock.
        public let wallMilliseconds: Double
        public let path: String
        /// The CPU's part: the encode closure, and the commit call (the raw path encodes and submits inside it).
        public var encodeMilliseconds = 0.0
        public var commitMilliseconds = 0.0
        /// The raw facade's clock of gpuMilliseconds (RawFacade.gpuClock); nil off the facade.
        public var gpuClock: String?

        /// GPU timestamps that advance prove the work ran on the GPU's clock, not a CPU fallback.
        public var gpuTimed: Bool { gpuMilliseconds > 0 }
    }

    /// Every command buffer `run` committed, by the path the raw facade reports for it ("device" elsewhere).
    public var paths: PathTally { locked { recorded } }

    /// Runs whose GPU time on the raw facade is the host's window, for want of a GPU stamp (RawFacade.gpuClock).
    public var hostClockedRuns: Int { locked { hostClocked } }

    /// Command buffers that completed with an error, in order (their error's domain, code and text).
    public var commandBufferErrors: [String] { locked { failures } }

    public func compute(_ name: String) throws -> MTLComputePipelineState {
        guard let function = library.makeFunction(name: name) else { throw TestFailure("no function \(name)") }
        return try device.makeComputePipelineState(function: function)
    }

    public func render(
        vertex: String = "quad_vertex", fragment: String, format: MTLPixelFormat, samples: Int = 1,
        depth: MTLPixelFormat = .invalid, stencil: MTLPixelFormat = .invalid,
        configure: (MTLRenderPipelineDescriptor) -> Void = { _ in }
    ) throws -> MTLRenderPipelineState {
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: vertex)
        descriptor.fragmentFunction = library.makeFunction(name: fragment)
        descriptor.colorAttachments[0].pixelFormat = format
        descriptor.rasterSampleCount = samples
        descriptor.depthAttachmentPixelFormat = depth
        descriptor.stencilAttachmentPixelFormat = stencil
        configure(descriptor)
        return try device.makeRenderPipelineState(descriptor: descriptor)
    }

    /// Encodes, commits and waits for one command buffer; throws its error if the GPU reports one (`tolerateError`
    /// returns it in the timing's path as "error:..." instead).
    @discardableResult
    public func run(tolerateError: Bool = false, _ encode: (MTLCommandBuffer) throws -> Void) async throws -> Timing {
        guard let buffer = queue.makeCommandBuffer() else { throw TestFailure("makeCommandBuffer returned nil") }
        let began = CFAbsoluteTimeGetCurrent()
        try encode(buffer)
        let start = CFAbsoluteTimeGetCurrent()
        var committed = start
        let completed = await withCheckedContinuation { (done: CheckedContinuation<Double, Never>) in
            buffer.addCompletedHandler { _ in done.resume(returning: CFAbsoluteTimeGetCurrent()) }
            buffer.commit()
            committed = CFAbsoluteTimeGetCurrent()
        }
        let wall = (completed - start) * 1000
        let encodeMs = (start - began) * 1000
        let commitMs = (min(committed, completed) - start) * 1000
        let path = RawFacade.path(of: buffer)
        locked { recorded.add(path) }
        if buffer.status == .error {
            let error = buffer.error.map(Self.describe) ?? "no error"
            locked { failures.append(error) }
            if !tolerateError { throw TestFailure("command buffer failed: \(error)") }
            return Timing(
                gpuMilliseconds: 0, wallMilliseconds: wall, path: "error:" + error, encodeMilliseconds: encodeMs,
                commitMilliseconds: commitMs)
        }
        let clock = RawFacade.gpuClock(of: buffer)
        if clock == "host" { locked { hostClocked += 1 } }
        return Timing(
            gpuMilliseconds: max(0, buffer.gpuEndTime - buffer.gpuStartTime) * 1000, wallMilliseconds: wall, path: path,
            encodeMilliseconds: encodeMs, commitMilliseconds: commitMs, gpuClock: clock)
    }

    /// Commits one command buffer without waiting: `finish` waits for a batch of them, so work queued back to back
    /// tests the order the GPU runs it in.
    public func commit(_ encode: (MTLCommandBuffer) throws -> Void) throws -> MTLCommandBuffer {
        guard let buffer = queue.makeCommandBuffer() else { throw TestFailure("makeCommandBuffer returned nil") }
        try encode(buffer)
        buffer.commit()
        return buffer
    }

    public func finish(_ buffers: [MTLCommandBuffer]) throws {
        var errors: [String] = []
        for buffer in buffers {
            buffer.waitUntilCompleted()
            let path = RawFacade.path(of: buffer)
            locked { recorded.add(path) }
            if buffer.status == .error { errors.append(buffer.error.map(Self.describe) ?? "no error") }
        }
        locked { failures += errors }
        if let first = errors.first { throw TestFailure("command buffer failed: \(first)") }
    }

    private func locked<T>(_ body: () -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body()
    }

    /// An error as "domain/code text", short enough for a log field.
    public static func describe(_ error: Error) -> String {
        let ns = error as NSError
        return "\(ns.domain)/\(ns.code) \(ns.localizedDescription)"
    }

    public func buffer(_ length: Int) throws -> MTLBuffer {
        guard let buffer = device.makeBuffer(length: max(16, length), options: .storageModeShared) else {
            throw TestFailure("makeBuffer(\(length)) returned nil")
        }
        return buffer
    }

    public func buffer<T>(_ values: [T]) throws -> MTLBuffer {
        let buffer = try buffer(values.count * MemoryLayout<T>.stride)
        values.withUnsafeBytes { buffer.contents().copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
        return buffer
    }

    public func texture(
        _ format: MTLPixelFormat, _ width: Int, _ height: Int, usage: MTLTextureUsage = [.renderTarget, .shaderRead],
        samples: Int = 1, storage: MTLStorageMode = .private, mipmaps: Int = 1
    ) throws -> MTLTexture {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: width, height: height, mipmapped: false)
        descriptor.usage = usage
        descriptor.storageMode = storage
        descriptor.sampleCount = samples
        descriptor.mipmapLevelCount = mipmaps
        if samples > 1 { descriptor.textureType = .type2DMultisample }
        guard let texture = device.makeTexture(descriptor: descriptor) else {
            throw TestFailure("makeTexture(\(format.rawValue) \(width)x\(height)) returned nil")
        }
        return texture
    }

    /// Copies a texture (any storage) into a shared buffer with the blit engine and returns its bytes, rows packed:
    /// level `level` of slice `slice`, or of a 3D texture's depth plane `plane`.
    public func read(
        _ texture: MTLTexture, bytesPerPixel: Int, level: Int = 0, slice: Int = 0, plane: Int = 0,
        options: MTLBlitOption = []
    ) async throws -> [UInt8] {
        let width = max(1, texture.width >> level)
        let height = max(1, texture.height >> level)
        let rowBytes = width * bytesPerPixel
        let out = try buffer(rowBytes * height)
        try await run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            let size = MTLSize(width: width, height: height, depth: 1)
            let origin = MTLOrigin(x: 0, y: 0, z: plane)
            // m2v's blit encoder has only the form without options: the other is answered zero and copies nothing
            if options.isEmpty {
                blit.copy(
                    from: texture, sourceSlice: slice, sourceLevel: level, sourceOrigin: origin, sourceSize: size,
                    to: out, destinationOffset: 0, destinationBytesPerRow: rowBytes, destinationBytesPerImage: 0)
            } else {
                blit.copy(
                    from: texture, sourceSlice: slice, sourceLevel: level, sourceOrigin: origin, sourceSize: size,
                    to: out, destinationOffset: 0, destinationBytesPerRow: rowBytes, destinationBytesPerImage: 0,
                    options: options)
            }
            blit.endEncoding()
        }
        return [UInt8](UnsafeRawBufferPointer(start: out.contents(), count: rowBytes * height))
    }

    /// The blit readback as a raster of `format`.
    public func raster(
        _ texture: MTLTexture, _ format: TexelFormat, level: Int = 0, slice: Int = 0, plane: Int = 0
    ) async throws
        -> Raster
    {
        let bytes = try await read(
            texture, bytesPerPixel: format.bytesPerTexel, level: level, slice: slice, plane: plane)
        return format.raster(bytes, width: max(1, texture.width >> level), height: max(1, texture.height >> level))
    }

    /// The same texels read by a compute kernel instead of the blit engine (a command buffer m2v draws whole on the
    /// raw facade): where the two readbacks disagree, the readback is what is broken, not the scene.
    public func texels(_ texture: MTLTexture) async throws -> [SIMD4<Float>] {
        let count = texture.width * texture.height
        let out = try buffer(count * 16)
        let pipeline = try compute("read_float_texture")
        try await run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: texture.width, height: texture.height, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
            encoder.endEncoding()
        }
        return Array(
            UnsafeBufferPointer(start: out.contents().bindMemory(to: SIMD4<Float>.self, capacity: count), count: count))
    }

    /// The compute readback as a raster of `channels` (texels' first channels; alpha alone for A8).
    public func computeRaster(_ texture: MTLTexture, _ format: TexelFormat) async throws -> Raster {
        let texels = try await texels(texture)
        let alphaOnly = format == .a8Unorm
        return Raster(width: texture.width, height: texture.height, channels: format.channels, levels: format.levels) {
            x, y, c in
            texels[y * texture.width + x][alphaOnly ? 3 : c]
        }
    }

    public func upload(
        _ bytes: [UInt8], to texture: MTLTexture, bytesPerRow: Int, level: Int = 0, slice: Int = 0
    )
        async throws
    {
        let staging = try buffer(bytes)
        let width = max(1, texture.width >> level)
        let height = max(1, texture.height >> level)
        try await run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: staging, sourceOffset: 0, sourceBytesPerRow: bytesPerRow, sourceBytesPerImage: 0,
                sourceSize: MTLSize(width: width, height: height, depth: 1), to: texture, destinationSlice: slice,
                destinationLevel: level, destinationOrigin: MTLOrigin())
            blit.endEncoding()
        }
    }

    /// Corners of an axis-aligned quad in clip space for quad_vertex's triangle strip.
    public static func quad(x0: Float, y0: Float, x1: Float, y1: Float, z: Float) -> [SIMD4<Float>] {
        [SIMD4(x0, y0, z, 1), SIMD4(x1, y0, z, 1), SIMD4(x0, y1, z, 1), SIMD4(x1, y1, z, 1)]
    }

    /// The quad covering pixels [x0, x1) x [y0, y1) of a width x height target (y down).
    public static func pixelQuad(
        _ x0: Int, _ y0: Int, _ x1: Int, _ y1: Int, width: Int, height: Int, z: Float = 0.5
    ) -> [SIMD4<Float>] {
        let fx = { (x: Int) in Float(x) / Float(width) * 2 - 1 }
        let fy = { (y: Int) in 1 - Float(y) / Float(height) * 2 }
        return quad(x0: fx(x0), y0: fy(y0), x1: fx(x1), y1: fy(y1), z: z)
    }

    public static let fullScreen = quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.5)

    public static func draw(_ encoder: MTLRenderCommandEncoder, corners: [SIMD4<Float>], color: SIMD4<Float>) {
        var corners = corners
        var color = color
        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
        encoder.setFragmentBytes(&color, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }

    public var proof: String { "\(device.name) registry=\(String(device.registryID, radix: 16))" }
}

extension Array where Element == UInt8 {
    public func pixel(_ x: Int, _ y: Int, width: Int, bytesPerPixel: Int = 4) -> ArraySlice<UInt8> {
        let at = (y * width + x) * bytesPerPixel
        return self[at..<(at + bytesPerPixel)]
    }

    public func loaded<T>(as type: T.Type, at index: Int) -> T {
        withUnsafeBytes { $0.loadUnaligned(fromByteOffset: index * MemoryLayout<T>.stride, as: T.self) }
    }
}
