import CoreMedia
import CoreVideo
import Foundation
import HWTestCore
import HWTestVideo
import Metal
import VideoToolbox

/// 720p NV12 pictures the GPU renders (a render pass clearing each plane, so GM20B writes every sample), handed to a
/// hardware H.264 session with the renders still in flight: NVENC reads each picture in place, its job ordered behind
/// the render by the GPU-to-host1x bridge (TegraNVENCDirect.cpp), with no VIC job and no CPU copy (SwitchEncoderRoutes
/// input_direct), or VIC copies the ones NVENC does not read as they are (input_vic; TegraNVENC's refusal line says
/// why). Every frame decodes back to the flat grey it was cleared to.
struct VideoGPUSourceTest: HWTest {
    let id = "encode.h264_from_gpu"
    let name = "H.264 720p from GPU-rendered NV12 pictures, encoded behind the render"
    let subsystem = Subsystem.encode
    var timeout: TimeInterval { 90 }
    static let width = 1280, height = 720, frames = 30

    /// Video-range luma of frame `i`: a step a frame, so a picture read before its render or from another frame shows.
    static func luma(_ i: Int) -> Int { 16 + 6 * i }

    func run(_ context: TestContext) async throws -> Outcome {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
            return .fail("no Metal device")
        }
        var pictures: [CVPixelBuffer] = []
        for i in 0..<Self.frames {
            guard let picture = Self.render(device, queue, luma: Self.luma(i)) else {
                return .fail("frame \(i): no NV12 IOSurface the GPU renders into")
            }
            pictures.append(picture)
        }
        let settings = CompressorSettings(
            codec: .h264, width: Self.width, height: Self.height, fps: 30, bitrate: 8_000_000, realTime: false,
            requireHardware: true)
        let encoded = try Compressor.encode(settings, pictures: pictures)
        let decoded = try Decompressor.decodePictures(
            encoded.samples, format: encoded.format, width: Self.width, height: Self.height, bitDepth: 8)
        var worst = 0.0
        for (i, picture) in decoded.pictures.enumerated() {
            worst = max(worst, abs(Self.meanLuma(picture) - Double(Self.luma(i))))
        }
        let routes = encoded.routes
        var metrics = Metrics()
        metrics.set("frames", Self.frames)
        for key in ["input_direct", "input_vic", "input_bounced", "input_cpu_copies", "input_refused"] {
            metrics.set(key, routes?[key] ?? -1)
        }
        metrics.set("decoded", decoded.pictures.count)
        metrics.set("luma_err", worst, digits: 1)
        return .judged(
            [
                ("UsingHardwareAcceleratedVideoEncoder", encoded.usingHardware),
                (
                    "NVENC read every GPU-rendered picture in place: no VIC job, no CPU copy",
                    routes?["input_direct"] == Self.frames && routes?["input_vic"] == 0
                        && routes?["input_cpu_copies"] == 0 && routes?["input_refused"] == 0
                ),
                ("every frame decodes back", decoded.pictures.count == Self.frames),
                ("every frame decodes to its grey within 3", worst <= 3),
            ], metrics, proof: "input_direct=\(routes?["input_direct"].map(String.init) ?? "-") of \(Self.frames)")
    }

    /// An NV12 IOSurface whose planes a render pass clears (luma to `luma`, chroma to neutral), committed and not
    /// waited for.
    static func render(_ device: MTLDevice, _ queue: MTLCommandQueue, luma: Int) -> CVPixelBuffer? {
        let attributes =
            [
                kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary, kCVPixelBufferMetalCompatibilityKey: true,
            ] as CFDictionary
        var made: CVPixelBuffer?
        let created = CVPixelBufferCreate(
            nil, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &made)
        guard created == kCVReturnSuccess, let picture = made,
            let surface = CVPixelBufferGetIOSurface(picture)?.takeUnretainedValue(),
            let buffer = queue.makeCommandBuffer()
        else { return nil }
        let planes: [(MTLPixelFormat, Int, Int, MTLClearColor)] = [
            (.r8Unorm, width, height, MTLClearColor(red: Double(luma) / 255, green: 0, blue: 0, alpha: 1)),
            (.rg8Unorm, width / 2, height / 2, MTLClearColor(red: 128.0 / 255, green: 128.0 / 255, blue: 0, alpha: 1)),
        ]
        for (plane, (format, w, h, colour)) in planes.enumerated() {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: format, width: w, height: h, mipmapped: false)
            descriptor.usage = [.renderTarget, .shaderRead]
            guard let texture = device.makeTexture(descriptor: descriptor, iosurface: surface, plane: plane) else {
                return nil
            }
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = colour
            buffer.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        }
        buffer.commit()
        return picture
    }

    static func meanLuma(_ picture: CVPixelBuffer) -> Double {
        CVPixelBufferLockBaseAddress(picture, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(picture, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddressOfPlane(picture, 0) else { return -1 }
        let rowBytes = CVPixelBufferGetBytesPerRowOfPlane(picture, 0)
        let rows = CVPixelBufferGetHeightOfPlane(picture, 0), columns = CVPixelBufferGetWidthOfPlane(picture, 0)
        var sum = 0
        for y in stride(from: 0, to: rows, by: 8) {
            let row = base.advanced(by: y * rowBytes).assumingMemoryBound(to: UInt8.self)
            for x in stride(from: 0, to: columns, by: 8) { sum += Int(row[x]) }
        }
        return Double(sum) / Double(((rows + 7) / 8) * ((columns + 7) / 8))
    }
}
