import CoreVideo
import Darwin
import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import HWTestVideo
import IOSurface
import Metal
import VideoToolbox

/// JobVisibility (bit 18): a pass that writes only a private texture ends Internal, with no L2 flush. A shared buffer
/// is L2-cached, so the blit into one that reads the texture back ends External, as does a blit into a no-copy buffer
/// over the process's own cached pages. One made L2-bypassing (the facade's switchMetalSharedBypassesL2: probe) ends
/// Volatile: a wait for idle and a frame-buffer flush, no L2 flush. The CPU reads the pass's colour from all three.
struct JobVisibilityPathTest: GPUPathTest {
    let id = "gpu.path.job_visibility"
    let name = "Internal jobs skip the L2 flush; Volatile and External ones put CPU-read bytes in DRAM"
    let counters = ["internal_jobs", "volatile_jobs", "external_jobs"]

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let target = try gpu.texture(.rgba8Unorm, 64, 64)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        let want = Array([UInt8]([51, 102, 153, 255]).repeated(64 * 64))
        let shared = Self.wrong(try await gpu.read(target, bytesPerPixel: 4), want)
        guard GPURates.bypassL2(gpu.device, true)
        else { return ["the facade cannot make an L2-bypassing shared buffer"] }
        let bypassing: [UInt8]
        do {
            defer { GPURates.bypassL2(gpu.device, false) }
            bypassing = try await gpu.read(target, bytesPerPixel: 4)
        }
        let volatile = Self.wrong(bypassing, want)
        let noCopy = Self.wrong(try await Self.noCopyRead(gpu, target), want)
        metrics.set("wrong_bytes_shared", shared)
        metrics.set("wrong_bytes_volatile", volatile)
        metrics.set("wrong_bytes_no_copy", noCopy)
        var failures: [String] = []
        if shared != 0 { failures.append("\(shared) bytes read back wrong through a shared buffer") }
        if volatile != 0 { failures.append("\(volatile) bytes read back wrong through an L2-bypassing shared buffer") }
        if noCopy != 0 { failures.append("\(noCopy) bytes read back wrong through a no-copy buffer") }
        return failures
    }

    /// The 64x64 RGBA8 texture blitted into a shared buffer over page-aligned memory this process allocated
    static func noCopyRead(_ gpu: GPU, _ target: MTLTexture) async throws -> [UInt8] {
        let length = 64 * 64 * 4
        let page = Int(getpagesize())
        let allocBytes = (length + page - 1) & ~(page - 1)
        var allocation: UnsafeMutableRawPointer?
        guard posix_memalign(&allocation, page, allocBytes) == 0, let memory = allocation else {
            throw TestFailure("aligned readback allocation failed")
        }
        memory.initializeMemory(as: UInt8.self, repeating: 0, count: allocBytes)
        guard
            let readback = gpu.device.makeBuffer(
                bytesNoCopy: memory, length: allocBytes, options: .storageModeShared,
                deallocator: { pointer, _ in free(pointer) }
            )
        else {
            free(memory)
            throw TestFailure("no-copy readback buffer creation failed")
        }
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: target, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: 64, height: 64, depth: 1), to: readback, destinationOffset: 0,
                destinationBytesPerRow: 64 * 4, destinationBytesPerImage: length
            )
            blit.endEncoding()
        }
        return Array(UnsafeRawBufferPointer(start: readback.contents(), count: length))
    }
}

/// LeanCopyChannel (bit 19): replaceRegion and getBytes of a texture go through the process's copy channel, made
/// copy-only, and the bytes round-trip.
struct LeanCopyPathTest: GPUPathTest {
    let id = "gpu.path.lean_copy"
    let name = "Texture uploads and readbacks run on a copy-only channel"
    let counters = ["lean_copy_jobs"]

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let width = 160, height = 48, row = width * 4
        let texture = try gpu.texture(.rgba8Unorm, width, height, usage: [.shaderRead], storage: .shared)
        let pattern = (0 ..< (row * height)).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 3) }
        let region = MTLRegionMake2D(0, 0, width, height)
        let before = GM20BFacade.counters(of: gpu.device)
        texture.replace(region: region, mipmapLevel: 0, withBytes: pattern, bytesPerRow: row)
        var back = [UInt8](repeating: 0, count: pattern.count)
        texture.getBytes(&back, bytesPerRow: row, from: region, mipmapLevel: 0)
        let after = GM20BFacade.counters(of: gpu.device)
        let uploads = (after["texture_uploads"] ?? 0) - (before["texture_uploads"] ?? 0)
        let readbacks = (after["texture_readbacks"] ?? 0) - (before["texture_readbacks"] ?? 0)
        let wrong = Self.wrong(back, pattern)
        metrics.set("uploads", uploads)
        metrics.set("readbacks", readbacks)
        metrics.set("wrong_bytes", wrong)
        var failures: [String] = []
        if uploads <= 0 || readbacks <= 0 { failures.append("the copy channel did not move the texture") }
        if wrong != 0 { failures.append("\(wrong) bytes changed on the round trip") }
        return failures
    }
}

/// ControlReserve (bit 20): one command buffer with more setBytes than any before it makes the runtime allocate new
/// control buffers from the kernel's reserve; every draw's colour comes from its own setBytes.
struct ControlReservePathTest: GPUPathTest {
    let id = "gpu.path.control_reserve"
    let name = "Control buffers come from the reserve and carry every draw's bytes"
    let counters = ["control_bos"]
    static let width = 64, height = 8, bytesPerDraw = 4096

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let width = Self.width, height = Self.height
        let target = try gpu.texture(.rgba8Unorm, width, height)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        var want = [UInt8](repeating: 0, count: width * height * 4)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            var payload = [Float](repeating: 0, count: Self.bytesPerDraw / 4)
            for y in 0 ..< height {
                for x in 0 ..< width {
                    let value = UInt8((x * 3 + y * 29) & 0xff)
                    payload[0] = Float(value) / 255
                    payload[1] = Float(255 - value) / 255
                    payload[2] = Float(y) / 255
                    payload[3] = 1
                    var corners = GPU.pixelQuad(x, y, x + 1, y + 1, width: width, height: height)
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.setFragmentBytes(&payload, length: Self.bytesPerDraw, index: 0)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    let at = (y * width + x) * 4
                    want[at ..< (at + 4)] = [value, 255 - value, UInt8(y), 255]
                }
            }
            encoder.endEncoding()
        }
        let wrong = Self.wrong(try await gpu.read(target, bytesPerPixel: 4), want)
        metrics.set("draws", width * height)
        metrics.set("wrong_bytes", wrong)
        return wrong == 0 ? [] : ["\(wrong) bytes of the draws' colours wrong"]
    }
}

/// CpuComplete (bit 24): a GPU fill of no-copy cached memory is maintained by the kernel over exactly its range at
/// completion, so the original pointer reads it without another contents call.
struct CpuCompletePathTest: GPUPathTest {
    let id = "gpu.path.cpu_complete"
    let name = "The kernel maintains a GPU write's cached range at completion"
    let counters = ["cpu_completes"]

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let length = 3 * Int(getpagesize())
        var allocation: UnsafeMutableRawPointer?
        guard posix_memalign(&allocation, Int(getpagesize()), length) == 0, let memory = allocation else {
            throw TestFailure("aligned no-copy allocation failed")
        }
        memory.initializeMemory(as: UInt8.self, repeating: 0x11, count: length)
        guard
            let buffer = gpu.device.makeBuffer(
                bytesNoCopy: memory, length: length, options: .storageModeShared,
                deallocator: { pointer, _ in free(pointer) }
            )
        else {
            free(memory)
            throw TestFailure("no-copy buffer creation failed")
        }
        let range = 100 ..< (length - 300)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.fill(buffer: buffer, range: range, value: 0x6b)
            blit.endEncoding()
        }
        var wrong = 0
        for offset in 0 ..< length {
            let want: UInt8 = range.contains(offset) ? 0x6b : 0x11
            if memory.load(fromByteOffset: offset, as: UInt8.self) != want { wrong += 1 }
        }
        metrics.set("wrong_bytes", wrong)
        return wrong == 0 ? [] : ["\(wrong) bytes wrong through the original pointer"]
    }
}

/// UseRanges (bit 25): small buffers share memory, and no job holds a use of a neighbour: the GPU writes each buffer
/// while the CPU rewrites its neighbours, and no buffer loses either side's bytes. Two routes, one arm each:
/// - Shared buffers. On the facade's VOL=1 route (shared_route "set": switchMetalBypassesL2) their write-combined
///   chunks are resource-set members the CPU never leases, so a job lists no use of them at all (shared_use_adds 0,
///   this process's facade count of uses that reached a job's own list). On the L2-cached route ("listed") their
///   chunk's lease takes it out of the set and each job lists its buffer's bytes, as the heap's.
/// - A Shared heap's buffers: its arena is CPU-cached and leased at each buffer's creation, which takes it out of the
///   set, so each job lists its buffer by that buffer's bytes (heap_ranged_uses; ranged_uses is system-wide).
struct UseRangesPathTest: GPUPathTest {
    let id = "gpu.path.use_ranges"
    let name = "Buffers sharing memory are named by their own bytes or by the set, so each keeps its own writes"
    let counters = ["ranged_uses"]
    static let length = 768

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let connection = GPUConnection()
        var failures: [String] = []
        let shared = try (0 ..< 8).map { _ in try gpu.buffer(Self.length) }
        let addsBefore = GM20BFacade.encodeProfile(of: gpu.device)["use_adds"]
        let pathsBefore = GPUPaths.read(connection)
        let sharedWrong = try Self.writeBeside(gpu, shared)
        let addsBetween = GM20BFacade.encodeProfile(of: gpu.device)["use_adds"]
        let pathsBetween = GPUPaths.read(connection)
        let descriptor = MTLHeapDescriptor()
        descriptor.size = 64 * 1024
        descriptor.storageMode = .shared
        guard let heap = gpu.device.makeHeap(descriptor: descriptor) else { throw TestFailure("makeHeap returned nil") }
        let inHeap = try (0 ..< 8).map { _ -> MTLBuffer in
            guard let buffer = heap.makeBuffer(length: Self.length, options: .storageModeShared) else {
                throw TestFailure("the heap's makeBuffer(\(Self.length)) returned nil")
            }
            return buffer
        }
        let heapWrong = try Self.writeBeside(gpu, inHeap)
        let addsAfter = GM20BFacade.encodeProfile(of: gpu.device)["use_adds"]
        let pathsAfter = GPUPaths.read(connection)
        metrics.set("wrong_bytes", sharedWrong + heapWrong)
        metrics.set("heap_wrong_bytes", heapWrong)
        if case .counts(let before) = pathsBefore, case .counts(let between) = pathsBetween,
           case .counts(let after) = pathsAfter
        {
            metrics.set("shared_ranged_uses", between.taken("ranged_uses", since: before))
            metrics.set("heap_ranged_uses", after.taken("ranged_uses", since: between))
        }
        let routes = shared.map { (GM20BFacade.ask($0, "switchMetalBypassesL2") as? NSNumber)?.boolValue }
        let route = routes.allSatisfy { $0 == true } ? "set" : routes.allSatisfy { $0 == false } ? "listed" : "unread"
        metrics.set("shared_route", route)
        if let before = addsBefore, let between = addsBetween, let after = addsAfter {
            let sharedAdds = Int(between - before)
            metrics.set("shared_use_adds", sharedAdds)
            metrics.set("heap_use_adds", Int(after - between))
            if route == "set" && sharedAdds != 0 {
                failures.append("Shared buffers in set-member chunks listed \(sharedAdds) uses of their own")
            }
            if route == "listed" && sharedAdds == 0 {
                failures.append("Shared buffers out of the set: no job listed them")
            }
            if route == "unread" { failures.append("the facade did not say which route the Shared buffers took") }
        } else {
            metrics.set("shared_use_adds", "unread")
            let why = KernelSysctl.isReleaseKernel()
                ? "the facade's encode profile has no use_adds (a release build)"
                : "no encode profile (build with --//bazel:prof_gpu)"
            PathParts.current?.skip(why, counters: counters)
        }
        if sharedWrong > 0 { failures.append("\(sharedWrong) bytes wrong across Shared buffers that share chunks") }
        if heapWrong > 0 { failures.append("\(heapWrong) bytes wrong across a Shared heap's buffers") }
        return failures
    }

    // The GPU fills the even buffers while the CPU rewrites each odd one beside it: the bytes wrong after both
    static func writeBeside(_ gpu: GPU, _ buffers: [MTLBuffer]) throws -> Int {
        let length = Self.length
        for (index, buffer) in buffers.enumerated() {
            buffer.contents().initializeMemory(as: UInt8.self, repeating: UInt8(index), count: length)
        }
        var committed: [MTLCommandBuffer] = []
        for (index, buffer) in buffers.enumerated() where index % 2 == 0 {
            committed.append(
                try gpu.commit { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.fill(buffer: buffer, range: 0 ..< length, value: UInt8(0x80 | index))
                    blit.endEncoding()
                }
            )
            buffers[index + 1].contents().initializeMemory(
                as: UInt8.self, repeating: UInt8(0x40 | index), count: length
            )
        }
        try gpu.finish(committed)
        var wrong = 0
        for (index, buffer) in buffers.enumerated() {
            let want = UInt8(index % 2 == 0 ? 0x80 | index : 0x40 | (index - 1))
            let bytes = UnsafeRawBufferPointer(start: buffer.contents(), count: length)
            wrong += bytes.filter { $0 != want }.count
        }
        return wrong
    }
}

/// NativeSurfaceView (bit 17): a hardware-decoded frame's planes, read by a compute kernel through CVMetalTextureCache,
/// come from the decoder's own block-linear picture through a native view, and give the bytes a CPU lock of the frame
/// converts to rows. The GPU reads both planes before the CPU's lock prepares the rows; a blit would read the pitch
/// planes instead.
/// gpu.path.native_surface_view_p010 does the same with HEVC Main 10's first picture: 'x420' planes as r16Unorm and
/// rg16Unorm over the decoder's 16-bit block-linear picture, each sample's 16 bits against the CPU's rows.
struct NativeSurfaceViewPathTest: GPUPathTest {
    let id: String
    let name: String
    let codec: VideoCodec
    let counters = ["native_tiled_views", "native_reads"]

    static let nv12 = NativeSurfaceViewPathTest(
        id: "gpu.path.native_surface_view",
        name: "Draws sample a decoded picture where the decoder put it, matching its CPU rows", codec: .h264
    )
    static let p010 = NativeSurfaceViewPathTest(
        id: "gpu.path.native_surface_view_p010",
        name:
        "Draws sample a decoded Main 10 picture's 16-bit planes where the decoder put them, matching its CPU rows",
        codec: .hevc10
    )

    func exercise(_ gpu: GPU, _ context: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        guard let clip = Clip.all.first(where: { $0.codec == codec && $0.height == 720 }) else {
            throw TestFailure("no 720p \(codec) clip")
        }
        let frame = try Self.decodeFirst(try await Demux.load(clip, from: context.media), clip)
        var made: CVMetalTextureCache?
        guard CVMetalTextureCacheCreate(nil, nil, gpu.device, nil, &made) == kCVReturnSuccess, let cache = made else {
            throw TestFailure("CVMetalTextureCacheCreate failed")
        }
        let wrong = try await Self.planesWrong(frame, through: cache, gpu, bitDepth: codec.bitDepth)
        var failures: [String] = []
        for (plane, count) in wrong.enumerated() {
            metrics.set("plane\(plane)_wrong_bytes", count)
            if count != 0 { failures.append("plane \(plane): \(count) bytes differ from the CPU's rows") }
        }
        metrics.set("textures", wrong.count)
        return failures
    }

    final class PlaneSampler {
        let pipeline: MTLComputePipelineState
        private var buffers: [MTLBuffer?] = [nil, nil]

        init(_ gpu: GPU) throws { pipeline = try gpu.compute("read_float_texture") }

        func buffer(_ gpu: GPU, plane: Int, bytes: Int) throws -> MTLBuffer {
            if let existing = buffers[plane], existing.length >= bytes { return existing }
            let made = try gpu.buffer(bytes)
            buffers[plane] = made
            return made
        }
    }

    /// Per plane, the bytes a compute kernel samples from `frame` through `cache` that differ from its CPU rows; the GPU
    /// reads both planes before the CPU's lock
    static func planesWrong(
        _ frame: CVPixelBuffer, through cache: CVMetalTextureCache, _ gpu: GPU, bitDepth: Int,
        sampler: PlaneSampler? = nil
    ) async throws -> [Int] {
        let sampler = try sampler ?? PlaneSampler(gpu)
        let sampleBytes = bitDepth > 8 ? 2 : 1
        let planes: [(format: MTLPixelFormat, channels: Int)] =
            sampleBytes == 2 ? [(.r16Unorm, 1), (.rg16Unorm, 2)] : [(.r8Unorm, 1), (.rg8Unorm, 2)]
        var held: [CVMetalTexture] = []
        var outputs: [(buffer: MTLBuffer, width: Int, height: Int, channels: Int)] = []
        var committed: [MTLCommandBuffer] = []
        for (plane, shape) in planes.enumerated() {
            let width = CVPixelBufferGetWidthOfPlane(frame, plane)
            let height = CVPixelBufferGetHeightOfPlane(frame, plane)
            var texture: CVMetalTexture?
            guard
                CVMetalTextureCacheCreateTextureFromImage(
                    nil, cache, frame, nil, shape.format, width, height, plane, &texture
                ) == kCVReturnSuccess,
                let texture, let metal = CVMetalTextureGetTexture(texture)
            else { throw TestFailure("no Metal texture of plane \(plane)") }
            held.append(texture)
            let out = try sampler.buffer(gpu, plane: plane, bytes: width * height * 16)
            outputs.append((out, width, height, shape.channels))
            committed.append(
                try gpu.commit { commands in
                    let encoder = commands.makeComputeCommandEncoder()!
                    encoder.setComputePipelineState(sampler.pipeline)
                    encoder.setTexture(metal, index: 0)
                    encoder.setBuffer(out, offset: 0, index: 0)
                    encoder.dispatchThreads(
                        MTLSize(width: width, height: height, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
                    )
                    encoder.endEncoding()
                }
            )
        }
        try gpu.finish(committed)
        withExtendedLifetime(held) {}
        CVPixelBufferLockBaseAddress(frame, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(frame, .readOnly) }
        return outputs.enumerated().map { plane, output in
            guard let base = CVPixelBufferGetBaseAddressOfPlane(frame, plane) else {
                return output.width * output.height * output.channels * sampleBytes
            }
            let sampled = output.buffer.contents().bindMemory(
                to: SIMD4<Float>.self, capacity: output.width * output.height
            )
            return NativePlaneCompare.wrongBytes(
                sampled: sampled, planeBase: base, width: output.width, height: output.height,
                bytesPerRow: CVPixelBufferGetBytesPerRowOfPlane(frame, plane), channels: output.channels,
                sampleBytes: sampleBytes
            )
        }
    }

    final class Decoded: @unchecked Sendable {
        var image: CVImageBuffer?
        var status: OSStatus = noErr
    }

    static func decodeFirst(_ demuxed: Demuxed, _ clip: Clip) throws -> CVPixelBuffer {
        let specification: [CFString: Any] = [
            kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: true,
        ]
        let destination = PatternPixels.attributes(
            width: clip.width, height: clip.height, format: PatternPixels.format(bitDepth: clip.codec.bitDepth)
        )
        var made: VTDecompressionSession?
        let created = VTDecompressionSessionCreate(
            allocator: nil, formatDescription: demuxed.format, decoderSpecification: specification as CFDictionary,
            imageBufferAttributes: destination as CFDictionary, outputCallback: nil, decompressionSessionOut: &made
        )
        guard created == noErr, let session = made, let first = demuxed.samples.first else {
            throw TestFailure("VTDecompressionSessionCreate \(created)")
        }
        defer { VTDecompressionSessionInvalidate(session) }
        let decoded = Decoded()
        let status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: first, flags: [], infoFlagsOut: nil) {
            status, _, image, _, _ in
            decoded.status = status
            decoded.image = image
        }
        _ = VTDecompressionSessionWaitForAsynchronousFrames(session)
        guard status == noErr, decoded.status == noErr, let image = decoded.image else {
            throw TestFailure("decoding the first frame failed (\(status), \(decoded.status))")
        }
        return image
    }

    /// A plane as a CPU lock gives it, rows packed
    static func rows(_ frame: CVPixelBuffer, plane: Int, bytesPerPixel: Int) -> [UInt8] {
        CVPixelBufferLockBaseAddress(frame, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(frame, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddressOfPlane(frame, plane) else { return [] }
        let stride = CVPixelBufferGetBytesPerRowOfPlane(frame, plane)
        let rowBytes = CVPixelBufferGetWidthOfPlane(frame, plane) * bytesPerPixel
        return (0 ..< CVPixelBufferGetHeightOfPlane(frame, plane)).flatMap { y in
            [UInt8](UnsafeRawBufferPointer(start: base + y * stride, count: rowBytes))
        }
    }
}

/// SemanticFence (bit 22): the runtime reads every command buffer's completion through the fence record's semantic half
/// as well as its seqno; 48 command buffers back to back each fill their own slice and complete, and none reads as failed
/// or makes the raw channel again, as a failure misread as a prefix of later work would.
struct SemanticFencePathTest: GPUPathTest {
    let id = "gpu.path.semantic_fence"
    let name = "Completions read the fence record's semantic state, and no failure spills onto later work"
    let counters: [String] = []
    static let count = 48, slice = 256

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let buffer = try gpu.buffer(Self.count * Self.slice)
        let before = GM20BFacade.counters(of: gpu.device)
        var committed: [MTLCommandBuffer] = []
        for index in 0 ..< Self.count {
            committed.append(
                try gpu.commit { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.fill(
                        buffer: buffer, range: (index * Self.slice) ..< ((index + 1) * Self.slice),
                        value: UInt8(index + 1)
                    )
                    blit.endEncoding()
                }
            )
        }
        try gpu.finish(committed)
        let reopens = (GM20BFacade.counters(of: gpu.device)["reopens"] ?? 0) - (before["reopens"] ?? 0)
        let bytes = UnsafeRawBufferPointer(start: buffer.contents(), count: Self.count * Self.slice)
        var wrong = 0
        for (offset, byte) in bytes.enumerated() where byte != UInt8(offset / Self.slice + 1) {
            wrong += 1
        }
        metrics.set("reopens", reopens)
        metrics.set("wrong_bytes", wrong)
        var failures: [String] = []
        if reopens != 0 { failures.append("the raw channel was made again \(reopens) times") }
        if wrong != 0 { failures.append("\(wrong) bytes wrong") }
        return failures
    }
}

/// SharedEvents (bit 21): one queue's command buffer waits for a shared event on the GPU before it copies a buffer,
/// another's fills that buffer and signals the event on the GPU; the copy sees the fill, and both jobs carry the event
/// to the kernel.
struct SharedEventsPathTest: GPUPathTest {
    let id = "gpu.path.shared_events"
    let name = "A GPU wait on a shared event orders a copy after another queue's GPU signal"
    let counters = ["event_jobs"]

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        guard let event = gpu.device.makeSharedEvent(), let other = gpu.device.makeCommandQueue(),
              let waiter = other.makeCommandBuffer()
        else { throw TestFailure("no shared event or second queue") }
        let length = 4096
        let source = try gpu.buffer(length), target = try gpu.buffer(length)
        waiter.encodeWaitForEvent(event, value: 1)
        let copy = waiter.makeBlitCommandEncoder()!
        copy.copy(from: source, sourceOffset: 0, to: target, destinationOffset: 0, size: length)
        copy.endEncoding()
        waiter.commit()
        let producer = try gpu.commit { commands in
            let fill = commands.makeBlitCommandEncoder()!
            fill.fill(buffer: source, range: 0 ..< length, value: 0x3c)
            fill.endEncoding()
            commands.encodeSignalEvent(event, value: 1)
        }
        try gpu.finish([producer])
        waiter.waitUntilCompleted()
        var failures: [String] = []
        let waiterPath = GM20BFacade.path(of: waiter)
        metrics.set("waiter_path", waiterPath)
        if waiterPath != "gm20b" && waiterPath != "device" {
            failures.append("the waiting command buffer went \(waiterPath)")
        }
        if waiter.status != .completed { failures.append("the waiting command buffer ended \(waiter.status.rawValue)") }
        let wrong = UnsafeRawBufferPointer(start: target.contents(), count: length).filter { $0 != 0x3c }.count
        metrics.set("wrong_bytes", wrong)
        if wrong != 0 { failures.append("\(wrong) copied bytes miss the signalled fill") }
        metrics.set("signaled", Int(event.signaledValue))
        return failures
    }
}

/// A render into an IOSurface while the CPU holds a write lock on it (bit 23 advertised). Apple's contract
/// (IOSurfaceRef.h: "lock" is used loosely, a bound on CPU access; IOSurface.kext's lock waits for GPU work submitted
/// before it and never holds back GPU work after it) is that the render runs and its pixels are what the next lock
/// reads where the CPU wrote none. Reported apart: whether the kernel refused the submission (`kernel_refused`, a
/// ResourceReady readiness arm), whether the runtime held the command buffer back while the lock was held
/// (`held_back`), and the pixels. Only the pixels and the command buffer's completion decide it.
struct ResourceReadyPathTest: GPUPathTest {
    let id = "gpu.path.resource_ready"
    let name = "A render into a CPU-locked IOSurface runs and its pixels survive the unlock"
    let counters: [String] = []

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let width = 64, height = 64, rowBytes = 256
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: width, .height: height, .bytesPerElement: 4, .bytesPerRow: rowBytes,
            .allocSize: rowBytes * height, .pixelFormat: 0x4247_5241,
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface creation failed") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no texture over the IOSurface")
        }
        let connection = GPUConnection()
        let pathsBefore = GPUPaths.read(connection)
        guard surface.lock(options: [], seed: nil) == 0 else { throw TestFailure("IOSurfaceLock failed") }
        let render = try gpu.commit { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 1, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        try await Task.sleep(nanoseconds: 100_000_000)
        let heldBack = render.status != .completed && render.status != .error
        surface.unlock(options: [], seed: nil)
        try gpu.finish([render])
        metrics.set("held_back", heldBack)
        if case .counts(let before) = pathsBefore, case .counts(let after) = GPUPaths.read(connection) {
            metrics.set("kernel_refused", after.taken("ready_arms", since: before))
        } else {
            metrics.set("kernel_refused", "unreadable")
        }
        guard surface.lock(options: .readOnly, seed: nil) == 0 else {
            throw TestFailure("IOSurfaceLock for the readback failed")
        }
        var wrong = 0
        for y in 0 ..< height {
            let row = UnsafeRawBufferPointer(start: surface.baseAddress + y * rowBytes, count: width * 4)
            for x in 0 ..< width where Array(row[(x * 4) ..< (x * 4 + 4)]) != [0, 255, 0, 255] { wrong += 1 }
        }
        surface.unlock(options: .readOnly, seed: nil)
        metrics.set("wrong_pixels", wrong)
        var failures: [String] = []
        if render.status != .completed { failures.append("the render ended \(render.status.rawValue)") }
        if wrong != 0 { failures.append("\(wrong) pixels miss the render made under the lock") }
        return failures
    }
}

enum GPUPathTests {
    static let all: [any HWTest] = [
        JobVisibilityPathTest(), LeanCopyPathTest(), ControlReservePathTest(), CpuCompletePathTest(),
        UseRangesPathTest(), NativeSurfaceViewPathTest.nv12, NativeSurfaceViewPathTest.p010, SemanticFencePathTest(),
        SharedEventsPathTest(),
        ResourceReadyPathTest(), LockBetweenJobsPathTest(), SubmitJobPathTest(), RelayoutPathTest(),
        QuietJobsPathTest(), PresentPromisePathTest(), PromiseCancelPathTest(), StateSegmentsPathTest(),
        EncodeCostPathTest(), PromiseVICPathTest(), PromiseUnkeptPathTest(), PromiseReleaseOrderPathTest(),
        SetSurvivesTrimPathTest(), SurfaceLocksSideBySideTest.blockLinear, SurfaceLocksSideBySideTest.pitchLinear,
        ImportBesideSubmitsTest(), TransferBesideReadbackTest(), CommitLatencyPathTest(), AheadMovesTest(),
        FreshSurfaceImportPathTest(), NativeSurfaceRepublishedPathTest(), HolderTimeoutPathTest(),
        IdleRedrawPathTest(), LaunchTargetPathTest(), FreshSparePathTest(), TeardownFreePathTest(),
        ParkedBoPathTest(),
    ]
}

extension Array {
    fileprivate func repeated(_ times: Int) -> [Element] { (0 ..< times).flatMap { _ in self } }
}
