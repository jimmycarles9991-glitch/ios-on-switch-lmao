import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// Two-plane YCbCr textures smaller than their surface, of usage ShaderRead | ShaderWrite | RenderTarget, as
/// cameracaptured's FigMetalContext makes them (boot 21: RGB8_420_2P and YCBCR8_420_2P of 2x2 over an 8x8 '420f'
/// surface, then `Fig assert: "texture"`). Metal makes them and reads and stores their top-left corner (an M4 Max:
/// harness/ycbcroracle.mm's corner cases, and no byte outside the corner stored). Each target gets a clear and a draw:
/// the corner's bytes against Metal's store, every byte outside it as scribbled (outside_), the corner sampled against
/// the same bytes in a surface of the texture's own size (read_off_), every command buffer raw. Then one pass into a
/// YCBCR8_420_2P and an RGB8_420_2P corner at once, as cameracaptured's two-target states draw (an M4 Max stores each
/// as it stores it alone): two_wrong, two_outside.
public struct YcbcrCornerTest: HWTest {
    public let id = "gpu.ycbcr_corner"
    public let name = "YCbCr textures smaller than their surface: made, rendered and sampled at the corner, on the driver"
    public let subsystem = Subsystem.metal
    public var timeout: TimeInterval { 60 }

    public init() {}

    public struct Corner {
        public let format: UInt
        public let fourcc: OSType
        public let width: Int, height: Int, surfaceWidth: Int, surfaceHeight: Int

        public init(format: UInt, fourcc: OSType, width: Int, height: Int, surfaceWidth: Int, surfaceHeight: Int) {
            self.format = format
            self.fourcc = fourcc
            self.width = width
            self.height = height
            self.surfaceWidth = surfaceWidth
            self.surfaceHeight = surfaceHeight
        }

        public var key: String { "\(format)_\(width)x\(height)of\(surfaceWidth)" }
        public var converted: Bool { format == 500 }
        public var full: Bool { fourcc == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange }
    }

    public static let corners = [
        Corner(
            format: 540, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, width: 2, height: 2, surfaceWidth: 8,
            surfaceHeight: 8
        ),
        Corner(
            format: 500, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, width: 2, height: 2, surfaceWidth: 8,
            surfaceHeight: 8
        ),
        Corner(
            format: 540, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, width: 4, height: 2,
            surfaceWidth: 16, surfaceHeight: 16
        ),
        Corner(
            format: 500, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, width: 6, height: 4,
            surfaceWidth: 16, surfaceHeight: 16
        ),
    ]
    public static let usage: MTLTextureUsage = [.shaderRead, .shaderWrite, .renderTarget]

    public func run(_ context: TestContext) async throws -> Outcome {
        let probe = try GPU.make()
        guard GM20BFacade.kind(of: probe.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: probe.device)))")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for corner in Self.corners {
            let gpu = try GPU.make()
            var result = Result()
            do {
                result = try await exercise(gpu, corner)
            } catch {
                result.note = "failed: \(GPU.describe(error))"
            }
            let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "gm20b" }
            metrics.set("paths_" + corner.key, gpu.paths.isEmpty ? "-" : gpu.paths.text)
            metrics.set("wrong_" + corner.key, "\(result.luma)/\(result.chroma)")
            metrics.set("outside_" + corner.key, result.outside)
            metrics.set("read_off_" + corner.key, result.readOff)
            var check = "\(corner.key): made, every corner byte Metal's, none outside it, read as its own surface, raw"
            if let note = result.note { check += " (\(note))" }
            let off = result.luma + result.chroma + result.outside + result.readOff
            checks.append((check, result.note == nil && off == 0 && raw))
        }
        let gpu = try GPU.make()
        var two = Result()
        do {
            two = try await twoTargets(gpu)
        } catch {
            two.note = "failed: \(GPU.describe(error))"
        }
        let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "gm20b" }
        metrics.set("two_wrong", "\(two.luma)/\(two.chroma)")
        metrics.set("two_outside", two.outside)
        metrics.set("two_paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        var check = "500 and 540 corners in one pass: each as Metal stores it alone, raw"
        if let note = two.note { check += " (\(note))" }
        checks.append((check, two.note == nil && two.luma + two.chroma + two.outside == 0 && raw))
        return .judged(checks, metrics, proof: probe.proof)
    }

    func twoTargets(_ gpu: GPU) async throws -> Result {
        var result = Result()
        let corners = Array(Self.corners.prefix(2))
        var buffers: [CVPixelBuffer] = []
        var textures: [MTLTexture] = []
        for corner in corners {
            guard let buffer = Self.pixelBuffer(corner.surfaceWidth, corner.surfaceHeight, corner.fourcc),
                  let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
                  let format = MTLPixelFormat(rawValue: corner.format)
            else { throw TestFailure("no surface") }
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: format, width: corner.width, height: corner.height, mipmapped: false
            )
            descriptor.usage = Self.usage
            guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
                result.note = "no texture"
                return result
            }
            YcbcrTargetTest.scribble(buffer, seed: UInt8(textures.count + 5))
            buffers.append(buffer)
            textures.append(texture)
        }
        let state = try gpu.render(fragment: "texel_values_two", format: textures[0].pixelFormat) {
            $0.colorAttachments[1].pixelFormat = textures[1].pixelFormat
        }
        let (w, h) = (corners[0].width, corners[0].height)
        let values = YcbcrTargetTest.values(w * h)
        let before = buffers.map(Self.planes)
        let data = try gpu.buffer(values)
        var width = UInt32(w)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            for (index, texture) in textures.enumerated() {
                pass.colorAttachments[index].texture = texture
                pass.colorAttachments[index].loadAction = .dontCare
                pass.colorAttachments[index].storeAction = .store
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBuffer(data, offset: 0, index: 0)
            encoder.setFragmentBytes(&width, length: 4, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        for (index, corner) in corners.enumerated() {
            let texels = index == 0 ? values : values.map { SIMD4<Float>($0.z, $0.x, $0.y, $0.w) }
            let want = Self.codes(corner, texels, .drawnFloat)
            let after = Self.planes(buffers[index])
            for plane in 0 ..< 2 {
                let rows = plane == 0 ? h : h / 2
                for y in 0 ..< after[plane].count {
                    for x in 0 ..< after[plane][y].count {
                        if y < rows && x < w {
                            let wrong = UInt16(after[plane][y][x]) != want[plane][y * w + x] ? 1 : 0
                            if plane == 0 { result.luma += wrong } else { result.chroma += wrong }
                        } else {
                            result.outside += after[plane][y][x] == before[index][plane][y][x] ? 0 : 1
                        }
                    }
                }
            }
        }
        return result
    }

    struct Result {
        var luma = 0, chroma = 0, outside = 0, readOff = 0
        var note: String?
    }

    func exercise(_ gpu: GPU, _ corner: Corner) async throws -> Result {
        var result = Result()
        guard let buffer = Self.pixelBuffer(corner.surfaceWidth, corner.surfaceHeight, corner.fourcc),
              let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
              let format = MTLPixelFormat(rawValue: corner.format)
        else {
            result.note = "no surface"
            return result
        }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: corner.width, height: corner.height, mipmapped: false
        )
        descriptor.usage = Self.usage
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            result.note = "no texture"
            return result
        }
        let floats = try gpu.render(fragment: "texel_values", format: format)
        let (w, h) = (corner.width, corner.height)
        let values = YcbcrTargetTest.values(w * h)
        let c = YcbcrTargetTest.clear
        let clear = SIMD4<Float>(Float(c.red), Float(c.green), Float(c.blue), 1)
        let passes: [(MTLRenderPipelineState?, [SIMD4<Float>], YcbcrStore.Source)] = [
            (nil, Array(repeating: clear, count: w * h), .clear), (floats, values, .drawnFloat),
        ]
        for (index, (state, texels, source)) in passes.enumerated() {
            YcbcrTargetTest.scribble(buffer, seed: UInt8(index))
            let before = Self.planes(buffer)
            let data = try gpu.buffer(values)
            var width = UInt32(w)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = state == nil ? .clear : .dontCare
                pass.colorAttachments[0].clearColor = c
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                if let state {
                    encoder.setRenderPipelineState(state)
                    var corners = GPU.fullScreen
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.setFragmentBuffer(data, offset: 0, index: 0)
                    encoder.setFragmentBytes(&width, length: 4, index: 1)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            let after = Self.planes(buffer)
            let want = Self.codes(corner, texels, source)
            for plane in 0 ..< 2 {
                let rows = plane == 0 ? h : h / 2
                for y in 0 ..< after[plane].count {
                    for x in 0 ..< after[plane][y].count {
                        if y < rows && x < w {
                            let wrong = UInt16(after[plane][y][x]) != want[plane][y * w + x] ? 1 : 0
                            if plane == 0 { result.luma += wrong } else { result.chroma += wrong }
                        } else {
                            result.outside += after[plane][y][x] == before[plane][y][x] ? 0 : 1
                        }
                    }
                }
            }
        }
        result.readOff = try await readOff(gpu, corner, texture, buffer)
        return result
    }

    /// The corner's texels a draw reads, against the same bytes in a surface of the texture's own size.
    func readOff(_ gpu: GPU, _ corner: Corner, _ texture: MTLTexture, _ buffer: CVPixelBuffer) async throws -> Int {
        let (w, h) = (corner.width, corner.height)
        guard let own = Self.pixelBuffer(w, h, corner.fourcc),
              let surface = CVPixelBufferGetIOSurface(own)?.takeUnretainedValue()
        else { throw TestFailure("no \(w)x\(h) surface") }
        Self.copyCorner(from: buffer, to: own, width: w, height: h)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: texture.pixelFormat, width: w, height: h, mipmapped: false
        )
        descriptor.usage = Self.usage
        guard let twin = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no texture over a \(w)x\(h) surface")
        }
        let state = try gpu.render(fragment: "texel_read", format: .rgba32Float)
        var reads: [[UInt8]] = []
        for source in [texture, twin] {
            let target = try gpu.texture(.rgba32Float, w, h)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentTexture(source, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            reads.append(try await gpu.read(target, bytesPerPixel: 16))
        }
        return (0 ..< (w * h)).filter { reads[0][16 * $0 ..< 16 * $0 + 16] != reads[1][16 * $0 ..< 16 * $0 + 16] }.count
    }

    /// Plane 0's codes of the corner, then plane 1's Cb, Cr pairs.
    public static func codes(_ corner: Corner, _ texels: [SIMD4<Float>], _ source: YcbcrStore.Source) -> [[UInt16]] {
        let (luma, chroma) =
            corner.converted
                ? YcbcrStore.converted(
                    texels, width: corner.width, height: corner.height, full: corner.full, halvedDown: true,
                    source: source
                )
                : YcbcrStore.codes(
                    texels, width: corner.width, height: corner.height, max: 255, halvedDown: true, source: source
                )
        return [luma, chroma]
    }

    public static func pixelBuffer(_ width: Int, _ height: Int, _ fourcc: OSType) -> CVPixelBuffer? {
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        return CVPixelBufferCreate(nil, width, height, fourcc, attributes, &made) == kCVReturnSuccess ? made : nil
    }

    /// Each plane's rows, the plane's own bytes of each (a luma code, or a Cb, Cr pair, a byte each).
    public static func planes(_ buffer: CVPixelBuffer) -> [[[UInt8]]] {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        return (0 ..< 2).map { plane in
            guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { return [] }
            let row = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
            let bytes = CVPixelBufferGetWidthOfPlane(buffer, plane) * (plane == 0 ? 1 : 2)
            let bound = base.bindMemory(to: UInt8.self, capacity: row * CVPixelBufferGetHeightOfPlane(buffer, plane))
            return (0 ..< CVPixelBufferGetHeightOfPlane(buffer, plane)).map { y in
                (0 ..< bytes).map { bound[y * row + $0] }
            }
        }
    }

    public static func copyCorner(from: CVPixelBuffer, to: CVPixelBuffer, width: Int, height: Int) {
        CVPixelBufferLockBaseAddress(from, .readOnly)
        CVPixelBufferLockBaseAddress(to, [])
        defer {
            CVPixelBufferUnlockBaseAddress(to, [])
            CVPixelBufferUnlockBaseAddress(from, .readOnly)
        }
        for plane in 0 ..< 2 {
            guard let source = CVPixelBufferGetBaseAddressOfPlane(from, plane),
                  let destination = CVPixelBufferGetBaseAddressOfPlane(to, plane)
            else { continue }
            let rows = plane == 0 ? height : height / 2
            for y in 0 ..< rows {
                (destination + y * CVPixelBufferGetBytesPerRowOfPlane(to, plane)).copyMemory(
                    from: source + y * CVPixelBufferGetBytesPerRowOfPlane(from, plane), byteCount: width
                )
            }
        }
    }
}
