import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// YCbCr targets whose shape is not their surface's (m2v-textures5 probes, an M4 Max), each a Clear (0.5, 0.25, 0.75, 1)
/// and Store pass over a surface made by IOSurfaceCreate (planes at 4 KiB offsets, every byte i * 31 + 7 first),
/// against the whole allocation as Apple leaves it: plane_one, 540 32x32 over plane 1 of a 64x64 '420v': bytes x < 32
/// of plane 1's rows 0..31 are 40, nothing else changes; taller, 500 64x128 over a 64x64 '420v': plane 0 all 64 (the
/// bytes after it are not judged: taller()); chroma_finer, 502 64x64 over the same: plane 0 all 64, bytes 4096..8191
/// the pairs af 93; chroma_coarser, 500 64x64 over a
/// '422v': plane 0 all 64, plane 1 rows 0..31 the pairs af 93, rows 32..63 kept. odd_blocks: 540 33x17, usage
/// ShaderRead, over a 64x64 '420v', a DontCare and Store pass drawing red (3y + x) & 255, green (x + 7y) & 255, blue
/// (4x + y) & 255 (codes / 255) over the texture: luma the green code inside, each 2x2 chroma block's Cb, Cr the
/// rounded-half-up mean of its texels inside (row 16 x 30..32: 142 143 144; blocks (15, 7) 137,74, (16, 7) 143,76,
/// (15, 8) 138,79, (16, 8) 144,80), block row 9 and everything outside kept. <case>_status: 0 completed, 1 the command
/// buffer failed, 2 no texture; <case>_bytes_off: bytes not Apple's (0 each).
struct YcbcrTargetShapesTest: HWTest {
    let id = "gpu.gm20b.ycbcr_target_shapes"
    let name = "YCbCr targets over a plane 1, a shorter or differently subsampled surface, odd sizes, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    typealias Plane = (width: Int, height: Int, bytes: Int, pitch: Int)

    struct Shape {
        let key: String
        let format: UInt
        let width, height, plane: Int
        let fourcc: OSType
        let planes: [Plane]
        var drawn = false
        /// The bytes of the allocation after the pass (the second) that are not Apple's, from the one before it
        let off: ([UInt8], [UInt8]) -> Int
    }

    static let p420: [Plane] = [(64, 64, 1, 64), (32, 32, 2, 64)], p422: [Plane] = [(64, 64, 1, 64), (32, 64, 2, 64)]
    static let fourcc420 = OSType(0x3432_3076), fourcc422 = OSType(0x3432_3276)

    static func exact(_ expected: @escaping ([UInt8]) -> [UInt8]) -> ([UInt8], [UInt8]) -> Int {
        { before, after in zip(after, expected(before)).filter { $0 != $1 }.count }
    }

    /// taller: plane 0's rows 64..127 and the chroma of rows 0..127 both land in bytes 4096..8191, in the order the
    /// GPU writes them, and an M4 Max leaves them mixed (64 at 4096..6143 and 7168..8127, the pairs between, ef f7 in
    /// the last row), so only plane 0 is held to Apple's
    static func taller(_ before: [UInt8], _ after: [UInt8]) -> Int { (0 ..< 4096).filter { after[$0] != 0x64 }.count }

    static func filled(_ bytes: [UInt8], _ range: Range<Int>, _ pattern: [UInt8]) -> [UInt8] {
        var out = bytes
        for i in range { out[i] = pattern[(i - range.lowerBound) % pattern.count] }
        return out
    }

    static let shapes = [
        Shape(
            key: "plane_one", format: 540, width: 32, height: 32, plane: 1, fourcc: fourcc420, planes: p420,
            off: exact { b in
                var out = b
                for y in 0 ..< 32 { for x in 0 ..< 32 { out[4096 + y * 64 + x] = 0x40 } }
                return out
            }
        ),
        Shape(
            key: "taller", format: 500, width: 64, height: 128, plane: 0, fourcc: fourcc420, planes: p420,
            off: taller
        ),
        Shape(
            key: "chroma_finer", format: 502, width: 64, height: 64, plane: 0, fourcc: fourcc420, planes: p420,
            off: exact { filled(filled($0, 0 ..< 4096, [0x64]), 4096 ..< 8192, [0xaf, 0x93]) }
        ),
        Shape(
            key: "chroma_coarser", format: 500, width: 64, height: 64, plane: 0, fourcc: fourcc422, planes: p422,
            off: exact { filled(filled($0, 0 ..< 4096, [0x64]), 4096 ..< (4096 + 32 * 64), [0xaf, 0x93]) }
        ),
        Shape(
            key: "odd_blocks", format: 540, width: 33, height: 17, plane: 0, fourcc: fourcc420, planes: p420,
            drawn: true,
            off: exact(oddBlocks)
        ),
    ]

    static func codes(_ x: Int, _ y: Int) -> (red: Int, green: Int, blue: Int) {
        ((3 * y + x) & 255, (x + 7 * y) & 255, (4 * x + y) & 255)
    }

    static func oddBlocks(_ before: [UInt8]) -> [UInt8] {
        var out = before
        for y in 0 ..< 17 { for x in 0 ..< 33 { out[y * 64 + x] = UInt8(codes(x, y).green) } }
        for by in 0 ..< 9 {
            for bx in 0 ..< 17 {
                let inside = [(0, 0), (1, 0), (0, 1), (1, 1)].map { (2 * bx + $0.0, 2 * by + $0.1) }
                    .filter { $0.0 < 33 && $0.1 < 17 }
                let mean = { (sum: Int) in UInt8((sum + inside.count / 2) / inside.count) }
                out[4096 + by * 64 + 2 * bx] = mean(inside.map { codes($0.0, $0.1).blue }.reduce(0, +))
                out[4096 + by * 64 + 2 * bx + 1] = mean(inside.map { codes($0.0, $0.1).red }.reduce(0, +))
            }
        }
        return out
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for shape in Self.shapes {
            do {
                let (status, off) = try await Self.observe(gpu, shape)
                metrics.set(shape.key + "_status", status)
                metrics.set(shape.key + "_bytes_off", off)
                checks.append(("\(shape.key): completes with Apple's bytes", status == 0 && off == 0))
            } catch {
                checks.append(("\(shape.key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func surface(_ shape: Shape) throws -> IOSurfaceRef {
        var offset = 0
        var info: [[CFString: Any]] = []
        for p in shape.planes {
            info.append([
                kIOSurfacePlaneWidth: p.width, kIOSurfacePlaneHeight: p.height, kIOSurfacePlaneBytesPerElement: p.bytes,
                kIOSurfacePlaneBytesPerRow: p.pitch, kIOSurfacePlaneOffset: offset,
                kIOSurfacePlaneSize: p.pitch * p.height,
            ])
            offset += (p.pitch * p.height + 4095) / 4096 * 4096
        }
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: shape.planes[0].width, kIOSurfaceHeight: shape.planes[0].height,
            kIOSurfacePixelFormat: shape.fourcc, kIOSurfaceAllocSize: offset, kIOSurfacePlaneInfo: info,
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { throw TestFailure("no surface") }
        IOSurfaceLock(surface, [], nil)
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        for i in 0 ..< IOSurfaceGetAllocSize(surface) { base[i] = UInt8(truncatingIfNeeded: i * 31 + 7) }
        IOSurfaceUnlock(surface, [], nil)
        return surface
    }

    static func bytes(_ surface: IOSurfaceRef) -> [UInt8] {
        IOSurfaceLock(surface, .readOnly, nil)
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        return Array(UnsafeBufferPointer(start: base, count: IOSurfaceGetAllocSize(surface)))
    }

    /// The pass's status (0 completed, 1 failed, 2 no texture) and the bytes not Apple's
    static func observe(_ gpu: GPU, _ shape: Shape) async throws -> (Int, Int) {
        let surface = try surface(shape)
        let start = bytes(surface)
        let format = MTLPixelFormat(rawValue: shape.format)!
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: shape.width, height: shape.height, mipmapped: false
        )
        descriptor.usage = shape.drawn ? [.shaderRead] : [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: shape.plane)
        else { return (2, 0) }
        let state = shape.drawn ? try gpu.render(fragment: "texel_values", format: format) : nil
        let values = (0 ..< (shape.width * shape.height)).map { i -> SIMD4<Float> in
            let c = codes(i % shape.width, i / shape.width)
            return SIMD4(Float(c.red), Float(c.green), Float(c.blue), 255) / 255
        }
        let data = try gpu.buffer(values)
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = state == nil ? .clear : .dontCare
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.5, green: 0.25, blue: 0.75, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if let state {
                var corners = GPU.fullScreen
                var width = UInt32(shape.width)
                encoder.setRenderPipelineState(state)
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentBuffer(data, offset: 0, index: 0)
                encoder.setFragmentBytes(&width, length: 4, index: 1)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
        if timing.path.hasPrefix("error:") { return (1, 0) }
        return (0, shape.off(start, bytes(surface)))
    }
}
