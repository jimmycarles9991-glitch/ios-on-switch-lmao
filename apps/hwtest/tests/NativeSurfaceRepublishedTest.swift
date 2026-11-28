import CoreVideo
import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestVideo
import IOSurface
import Metal
import VideoToolbox

/// A decoder surface published again with a later picture is read through a fresh native view (74768d1f9): the raw
/// path keeps a surface's view while its seed holds, and the kernel refuses a job that reads a stale one, which now
/// fails the command buffer. The 720p H.264 clip's first 32 pictures decoded one at a time, each released before the
/// next, so the decoder's pool hands the same surfaces out again; each picture's planes are sampled through one
/// CVMetalTextureCache before the CPU's lock, as gpu.path.native_surface_view samples its one. Fields: frames=,
/// surfaces= (distinct IOSurfaces), republished= (pictures in a surface an earlier picture used), wrong_bytes= against
/// the CPU's rows, failed= command buffers that failed (a refusal), native_vetoes= encodes the facade stopped at the
/// commit for a surface changed since (pipeline_abandons.native).
struct NativeSurfaceRepublishedPathTest: GPUPathTest {
    let id = "gpu.path.native_surface_republished"
    let name = "Decoder surfaces published again are sampled through fresh native views, matching their CPU rows"
    let counters = ["native_tiled_views", "native_reads"]
    var timeout: TimeInterval { 180 }
    /// Twice SwitchNVDECPark's 16
    static let pictures = 32

    final class Pictures: @unchecked Sendable {
        private let lock = NSLock()
        private var held: [CVPixelBuffer] = []
        private(set) var failure: OSStatus = noErr

        func add(_ status: OSStatus, _ image: CVImageBuffer?) {
            lock.lock()
            defer { lock.unlock() }
            if status != noErr { failure = status } else if let image { held.append(image) }
        }

        func take() -> [CVPixelBuffer] {
            lock.lock()
            defer { lock.unlock() }
            let taken = held
            held = []
            return taken
        }
    }

    func exercise(_ gpu: GPU, _ context: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        guard let clip = Clip.all.first(where: { $0.codec == .h264 && $0.height == 720 }) else {
            throw TestFailure("no 720p H.264 clip")
        }
        let demuxed = try await Demux.load(clip, from: context.media)
        let destination = PatternPixels.attributes(
            width: clip.width, height: clip.height, format: PatternPixels.format(bitDepth: 8)
        )
        let specification = [kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: true] as CFDictionary
        var session: VTDecompressionSession?
        let created = VTDecompressionSessionCreate(
            allocator: nil, formatDescription: demuxed.format, decoderSpecification: specification,
            imageBufferAttributes: destination as CFDictionary, outputCallback: nil, decompressionSessionOut: &session
        )
        guard created == noErr, let session else { throw TestFailure("VTDecompressionSessionCreate \(created)") }
        defer { VTDecompressionSessionInvalidate(session) }
        var made: CVMetalTextureCache?
        guard CVMetalTextureCacheCreate(nil, nil, gpu.device, nil, &made) == kCVReturnSuccess, let cache = made else {
            throw TestFailure("CVMetalTextureCacheCreate failed")
        }
        let sampler = try NativeSurfaceViewPathTest.PlaneSampler(gpu)
        let counters = GM20BFacade.counters(of: gpu.device)
        let failedBefore = gpu.commandBufferErrors.count
        let pictures = Pictures()
        var seen = Set<IOSurfaceID>()
        var frames = 0, republished = 0, wrong = 0
        let samples = Array(demuxed.samples.prefix(Self.pictures))
        for (index, sample) in samples.enumerated() {
            let status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: sample, flags: [], infoFlagsOut: nil)
                {
                    status, _, image, _, _ in pictures.add(status, image)
                }
            guard status == noErr else { throw TestFailure("VTDecompressionSessionDecodeFrame \(index): \(status)") }
            if index == samples.count - 1 {
                VTDecompressionSessionFinishDelayedFrames(session)
                VTDecompressionSessionWaitForAsynchronousFrames(session)
            }
            for frame in pictures.take() {
                guard let surface = CVPixelBufferGetIOSurface(frame)?.takeUnretainedValue() else {
                    throw TestFailure("a decoded picture with no IOSurface")
                }
                if !seen.insert(IOSurfaceGetID(surface)).inserted { republished += 1 }
                frames += 1
                let failedSoFar = gpu.commandBufferErrors.count
                do {
                    wrong += try await NativeSurfaceViewPathTest.planesWrong(
                        frame, through: cache, gpu, bitDepth: 8, sampler: sampler
                    ).reduce(0, +)
                } catch {
                    if gpu.commandBufferErrors.count == failedSoFar { throw error }
                }
                CVMetalTextureCacheFlush(cache, 0)
            }
        }
        guard pictures.failure == noErr else { throw TestFailure("decoder output \(pictures.failure)") }
        let failed = gpu.commandBufferErrors.count - failedBefore
        let vetoes =
            (GM20BFacade.counters(of: gpu.device)["pipeline_abandons.native"] ?? 0)
            - (counters["pipeline_abandons.native"] ?? 0)
        metrics.set("frames", frames)
        metrics.set("surfaces", seen.count)
        metrics.set("republished", republished)
        metrics.set("wrong_bytes", wrong)
        metrics.set("failed", failed)
        metrics.set("native_vetoes", vetoes)
        var failures: [String] = []
        if frames != samples.count { failures.append("\(frames) of \(samples.count) pictures read") }
        if republished == 0 { failures.append("the decoder never handed a surface out again") }
        if failed != 0 { failures.append("\(failed) command buffers failed: \(gpu.commandBufferErrors.last ?? "-")") }
        if wrong != 0 { failures.append("\(wrong) sampled bytes differ from the CPU's rows") }
        return failures
    }
}
