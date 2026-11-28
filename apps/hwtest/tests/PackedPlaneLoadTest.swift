import CoreVideo
import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// Passes that load R10Unorm_PACKED and RG10Unorm_PACKED over a 'p420' surface's planes (before this such a pass went to
/// m2v: gm20b:pass.ycbcr). The driver fills the pass's stand-in with each code over 1023, as Metal reads the codes, and the
/// store gives each back. The plane is first written on the CPU with codes three to a word (bits 30-31 and the tail
/// slots 0); a pass that loads it and draws nothing stores the same words (identity_wrong_), and one that draws the left
/// half stores the drawn half's codes as the store targets hold them (YcbcrStore.code) and the right half's as they were
/// (partial_wrong_). PASS: no word off and every command buffer on the driver.
struct PackedPlaneLoadTest: HWTest {
    let id = "gpu.gm20b.packed_plane_load"
    let name = "Loaded passes into packed 10-bit surface planes, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let views: [PackedPlaneTargetTest.View] = [588, 589].map {
        PackedPlaneTargetTest.View(format: $0, fourcc: YcbcrPackedTargetTest.p420)
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var failures: [String] = []
        for view in Self.views {
            let key = String(view.format)
            do {
                let (identity, partial, first) = try await exercise(gpu, view)
                metrics.set("identity_wrong_" + key, identity)
                metrics.set("partial_wrong_" + key, partial)
                metrics.set("words_" + key, first)
                if identity + partial != 0 { failures.append("\(view.label): \(identity) + \(partial) words wrong") }
            } catch {
                failures.append("\(view.label): \(GPU.describe(error))")
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        if gpu.paths.isEmpty || !gpu.paths.paths.keys.allSatisfy({ $0 == "gm20b" }) {
            failures.append("command buffers off the driver: \(gpu.paths.text)")
        }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }

    func exercise(_ gpu: GPU, _ view: PackedPlaneTargetTest.View) async throws -> (Int, Int, String) {
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard
            CVPixelBufferCreate(
                nil, YcbcrTargetTest.width, YcbcrTargetTest.height, view.fourcc, attributes, &made
            ) == kCVReturnSuccess,
            let buffer = made, let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
            let format = MTLPixelFormat(rawValue: view.format)
        else { throw TestFailure("no surface") }
        let (w, h) = (
            CVPixelBufferGetWidthOfPlane(buffer, view.plane), CVPixelBufferGetHeightOfPlane(buffer, view.plane)
        )
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: w, height: h, mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: view.plane) else {
            throw TestFailure("no texture")
        }
        let perRow = w * view.channels
        var codes = (0 ..< (perRow * h)).map { UInt16(($0 % perRow * 7 + $0 / perRow * 13) % 1024) }
        Self.fill(buffer, view.plane, codes, rows: h)
        try await Self.loadedPass(gpu, texture, nil, [], w, h)
        var first = ""
        let identity = PackedPlaneTargetTest.mismatches(buffer, view.plane, codes, rows: h, &first)
        let floats = try gpu.render(fragment: "texel_values", format: format)
        let values = YcbcrTargetTest.values(w * h)
        try await Self.loadedPass(gpu, texture, floats, values, w, h)
        for y in 0 ..< h {
            for x in 0 ..< (w / 2) {
                for c in 0 ..< view.channels {
                    let v = values[y * w + x][c]
                    codes[y * perRow + view.channels * x + c] = UInt16(YcbcrStore.code(v, .drawnFloat, max: 1023))
                }
            }
        }
        var ignored = ""
        let partial = PackedPlaneTargetTest.mismatches(buffer, view.plane, codes, rows: h, &ignored)
        return (identity, partial, first)
    }

    /// The plane's rows as `codes` (a row's three to a little-endian word, every other bit 0)
    static func fill(_ buffer: CVPixelBuffer, _ plane: Int, _ codes: [UInt16], rows: Int) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { return }
        let pitch = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane), perRow = codes.count / rows
        for y in 0 ..< rows {
            for x in 0 ..< ((perRow + 2) / 3) {
                var word: UInt32 = 0
                for i in 0 ..< 3 where 3 * x + i < perRow {
                    word |= UInt32(codes[y * perRow + 3 * x + i]) << (10 * i)
                }
                base.storeBytes(of: word, toByteOffset: y * pitch + 4 * x, as: UInt32.self)
            }
        }
    }

    /// A pass that loads `texture` and stores it, drawing `values` over its left half with `state` (none: no draw)
    static func loadedPass(
        _ gpu: GPU, _ texture: MTLTexture, _ state: MTLRenderPipelineState?, _ values: [SIMD4<Float>], _ w: Int,
        _ h: Int
    ) async throws {
        let data = try gpu.buffer(values.isEmpty ? [SIMD4<Float>.zero] : values)
        var width = UInt32(w)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if let state {
                encoder.setRenderPipelineState(state)
                var corners = GPU.pixelQuad(0, 0, w / 2, h, width: w, height: h)
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentBuffer(data, offset: 0, index: 0)
                encoder.setFragmentBytes(&width, length: 4, index: 1)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
    }
}
