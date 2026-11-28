import CoreVideo
import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// A pass that loads a YCbCr target stores only the 32x32 tiles its draws reach (reach.h), as Apple's GPU does: the
/// conversion is lossy, so a tile stored again changes (an M4 Max: about 474 luma and 505 chroma bytes of a reached
/// tile). Each case loads a 64x48 '420f' surface of random codes (format 500, the 709 matrix, gpu.gm20b.ycbcr_load's
/// surface) and draws in one command buffer of its own: a draw of an indirect command buffer inheriting the encoder's
/// state into tile (1, 0) (icb); a quad patch at tessellation factor 1 inside tile (0, 1) (patch); a state with no
/// fragment function inside tile (1, 1), which reaches as a discarding draw does (no_fragment, measured on an M4 Max);
/// rasterization off, the vertex function storing to a buffer, reaching nothing (no_raster; stored_no_raster= the
/// buffer's words written, 3); and one indirect command buffer of a triangle in tile (0, 0) and a line in tile (1, 1)
/// (icb_mixed). Per case reached_changed_<case>= bytes changed in the tiles it reaches (above 0; 0 for no_raster, which
/// reaches none), untouched_changed_<case>= bytes changed elsewhere (0), path_<case>=; failed_cbs=.
struct ReachShapesTest: HWTest {
    let id = "gpu.gm20b.reach_shapes"
    let name = "Loaded YCbCr passes store the tiles an ICB, patch, fragmentless or unrasterized draw reaches, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct Out { float4 position [[position]]; };
    vertex Out reach_vertex(uint vid [[vertex_id]], constant float4 *corners [[buffer(0)]]) { return {corners[vid]}; }
    fragment float4 reach_colour() { return float4(0.3f, 0.6f, 0.2f, 1); }
    [[patch(quad, 4)]] vertex Out reach_patch(float2 uv [[position_in_patch]], constant float4 &box [[buffer(0)]]) {
        return {float4(mix(box.xy, box.zw, uv), 0, 1)};
    }
    vertex void reach_store(uint vid [[vertex_id]], device uint *out [[buffer(1)]]) { out[vid] = vid + 1; }
    """
    static let width = 64, height = 48
    static let surface = YcbcrLoadTest.Case(
        format: 500, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
        matrix: kCVImageBufferYCbCrMatrix_ITU_R_709_2, metal: 0
    )

    enum Shape: String, CaseIterable {
        case icb, patch, noFragment = "no_fragment", noRaster = "no_raster", icbMixed = "icb_mixed"

        /// The tiles (y * 2 + x) the case's draws reach
        var reached: Set<Int> {
            switch self {
            case .icb: return [1]
            case .patch: return [2]
            case .noFragment: return [3]
            case .noRaster: return []
            case .icbMixed: return [0, 3]
            }
        }
    }

    static func corners(_ x0: Int, _ y0: Int, _ x1: Int, _ y1: Int) -> [SIMD4<Float>] {
        GPU.pixelQuad(x0, y0, x1, y1, width: width, height: height)
    }

    func state(
        _ gpu: GPU, _ library: MTLLibrary, vertex: String, fragment: String?, icb: Bool
    ) async throws
        -> MTLRenderPipelineState
    {
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = library.makeFunction(name: vertex)
        d.fragmentFunction = fragment.flatMap { library.makeFunction(name: $0) }
        d.colorAttachments[0].pixelFormat = MTLPixelFormat(rawValue: Self.surface.format)!
        d.supportIndirectCommandBuffers = icb
        d.isRasterizationEnabled = fragment != nil || vertex != "reach_store"
        if vertex == "reach_patch" { d.tessellationPartitionMode = .integer }
        return try await gpu.device.makeRenderPipelineState(descriptor: d)
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        let (w, h) = (Self.width, Self.height)
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard CVPixelBufferCreate(nil, w, h, Self.surface.fourcc, attributes, &made) == kCVReturnSuccess,
              let buffer = made, let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
        else { throw TestFailure("no '420f' surface") }
        IOSurfaceSetValue(surface, "IOSurfaceYCbCrMatrix" as CFString, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: Self.surface.format)!, width: w, height: h, mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let target = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no format 500 texture")
        }
        let before = GM20BFacade.counters(of: gpu.device)
        let rows = YcbcrLoadTest.rows(Self.surface)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for (index, shape) in Shape.allCases.enumerated() {
            let key = shape.rawValue
            do {
                let planes = YcbcrLoadTest.fill(buffer, rows, seed: UInt64(index) * 977 + 5)
                let (path, stored) = try await draw(shape, library, gpu, into: target)
                let (reached, untouched) = Self.changed(shape.reached, rows, planes, YcbcrLoadTest.read(buffer, rows))
                metrics.set("reached_changed_\(key)", reached)
                metrics.set("untouched_changed_\(key)", untouched)
                metrics.set("path_\(key)", String(path.prefix(60)))
                if let stored { metrics.set("stored_\(key)", stored) }
                let reaches = shape.reached.isEmpty ? reached == 0 && stored == 3 : reached > 0
                checks.append(("\(key): the reached tiles stored, as Apple's are", reaches))
                checks.append(("\(key): no other tile stored", untouched == 0))
                checks.append(("\(key): on the driver", path == "gm20b"))
            } catch {
                checks.append(("\(key): \(GPU.describe(error))", false))
            }
        }
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// Encodes the case's pass into `target`; its command buffer's path and, for no_raster, the buffer words stored
    func draw(
        _ shape: Shape, _ library: MTLLibrary, _ gpu: GPU, into target: MTLTexture
    ) async throws
        -> (String, Int?)
    {
        let icb = shape == .icb || shape == .icbMixed
        let vertex = shape == .patch ? "reach_patch" : shape == .noRaster ? "reach_store" : "reach_vertex"
        let fragment = shape == .noFragment || shape == .noRaster ? nil : "reach_colour"
        let state = try await state(gpu, library, vertex: vertex, fragment: fragment, icb: icb)
        let points: [SIMD4<Float>] =
            switch shape {
            case .icb: Self.corners(40, 4, 60, 24)
            case .noFragment: Self.corners(36, 34, 60, 46)
            case .icbMixed: Array(Self.corners(4, 4, 28, 28)[0 ..< 3]) + Self.corners(36, 40, 60, 40)[2 ..< 4]
            default: Self.corners(0, 0, 4, 4)
            }
        let vertices = try gpu.buffer(points)
        let stores = try gpu.buffer([UInt32](repeating: 0, count: 4))
        let factors = try gpu.buffer([Float16](repeating: 1, count: 6))
        let patch = Self.corners(4, 36, 28, 44)
        var box = SIMD4<Float>(patch[0].x, patch[0].y, patch[3].x, patch[3].y)
        let commands = try indirect(shape, gpu)
        let path = try await gpu.run(tolerateError: true) { buffer in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            let encoder = buffer.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            encoder.setVertexBuffer(vertices, offset: 0, index: 0)
            switch shape {
            case .icb, .icbMixed:
                encoder.executeCommandsInBuffer(commands!, range: 0 ..< (shape == .icb ? 1 : 2))
            case .patch:
                encoder.setVertexBytes(&box, length: 16, index: 0)
                encoder.setTessellationFactorBuffer(factors, offset: 0, instanceStride: 0)
                encoder.drawPatches(
                    numberOfPatchControlPoints: 4, patchStart: 0, patchCount: 1, patchIndexBuffer: nil,
                    patchIndexBufferOffset: 0, instanceCount: 1, baseInstance: 0
                )
            case .noFragment:
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            case .noRaster:
                encoder.setVertexBuffer(stores, offset: 0, index: 1)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            }
            encoder.endEncoding()
        }.path
        let stored = UnsafeBufferPointer(start: stores.contents().bindMemory(to: UInt32.self, capacity: 4), count: 4)
            .filter { $0 != 0 }.count
        return (path, shape == .noRaster ? stored : nil)
    }

    /// The indirect command buffer of an icb case: a strip of 4 (icb), or a triangle then a line (icb_mixed)
    func indirect(_ shape: Shape, _ gpu: GPU) throws -> MTLIndirectCommandBuffer? {
        guard shape == .icb || shape == .icbMixed else { return nil }
        let d = MTLIndirectCommandBufferDescriptor()
        d.commandTypes = .draw
        d.inheritPipelineState = true
        d.inheritBuffers = true
        guard let icb = gpu.device.makeIndirectCommandBuffer(descriptor: d, maxCommandCount: 2, options: []) else {
            throw TestFailure("no indirect command buffer")
        }
        if shape == .icb {
            icb.indirectRenderCommandAt(0).drawPrimitives(
                .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: 1, baseInstance: 0
            )
        } else {
            icb.indirectRenderCommandAt(0).drawPrimitives(
                .triangle, vertexStart: 0, vertexCount: 3, instanceCount: 1, baseInstance: 0
            )
            icb.indirectRenderCommandAt(1).drawPrimitives(
                .line, vertexStart: 3, vertexCount: 2, instanceCount: 1, baseInstance: 0
            )
        }
        return icb
    }

    /// Bytes changed in the tiles `reached` and elsewhere (a chroma byte in its first luma texel's tile)
    static func changed(
        _ reached: Set<Int>, _ rows: [(bytes: Int, count: Int)], _ a: [[UInt8]], _ b: [[UInt8]]
    ) -> (reached: Int, untouched: Int) {
        var inside = 0, outside = 0
        for plane in 0 ..< 2 {
            for i in 0 ..< a[plane].count where a[plane][i] != b[plane][i] {
                let (y, x) = (i / rows[plane].bytes, i % rows[plane].bytes)
                let pixel = plane == 0 ? (x, y) : (x / 2 * 2, y * 2)
                if reached.contains(pixel.1 / 32 * 2 + pixel.0 / 32) { inside += 1 } else { outside += 1 }
            }
        }
        return (inside, outside)
    }
}
