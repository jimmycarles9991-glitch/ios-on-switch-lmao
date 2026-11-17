import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Draws a quad of `written_half_fragment` over `target`, writing `values` (one float4 a pixel, rows `width` apart) into
/// `written` at fragment texture index 1: the command buffer's path
func drawWrittenHalves(_ gpu: GPU, _ written: MTLTexture, _ values: [SIMD4<Float>], width: Int) async throws -> String {
    let target = try gpu.texture(.rgba8Unorm, written.width, written.height)
    let state = try gpu.render(fragment: "written_half_fragment", format: .rgba8Unorm)
    let buffer = try gpu.buffer(values)
    return try await gpu.run { commands in
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        e.setRenderPipelineState(state)
        var corners = GPU.fullScreen
        var w = UInt32(width)
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        e.setFragmentBuffer(buffer, offset: 0, index: 0)
        e.setFragmentBytes(&w, length: 4, index: 1)
        e.setFragmentTexture(written, index: 1)
        e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        e.endEncoding()
    }.path
}

/// A fragment function's storage write into a BGRA10_XR texture, which the raw path samples through an RGBA16Float stand-in
/// (before this the draw was refused: "a written texture the raw path holds through another image"). An 8x8 texture is
/// written with values across the extended range, then read by a second pass into an RGBA16Float target. The texture holds
/// each channel's code as Metal writes a half (raw_xr.h writeCode: 384 + round-half-even(min(|h|, 1.5) x 510) clamped,
/// alpha 384 + round(a x 510)), and the read gives (code - 384) / 510 rounded to half, so the stand-in was drawn again
/// after the write. PASS: no code or read off, both command buffers on the raw path.
struct WrittenXrTextureTest: HWTest {
    let id = "gpu.raw.written_xr_texture"
    let name = "A fragment function's storage write into an extended-range texture, read after, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let size = 8

    static func value(_ i: Int, _ k: Int) -> Float {
        k == 3 ? Float((i * 13) % 101) / 100 : -0.75 + 2 * Float(((i * 4 + k) * 37) % 101) / 100
    }

    /// The code Metal writes of `v` through half
    static func code(_ v: Float, alpha: Bool) -> Int {
        let h = Double(Float(Float16(v)))
        if alpha { return 384 + Int((min(max(h, 0), 1) * 510).rounded(.toNearestOrEven)) }
        let k = Int((min(abs(h), 1.5) * 510).rounded(.toNearestOrEven))
        return max(0, min(1023, h < 0 ? 384 - k : 384 + k))
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let n = Self.size
        let xr = try gpu.texture(.bgra10_xr, n, n, usage: [.shaderRead, .shaderWrite])
        let values: [SIMD4<Float>] = (0..<(n * n)).map { (i: Int) -> SIMD4<Float> in
            SIMD4<Float>(Self.value(i, 0), Self.value(i, 1), Self.value(i, 2), Self.value(i, 3))
        }
        var paths = [try await drawWrittenHalves(gpu, xr, values, width: n)]
        let target = try gpu.texture(.rgba16Float, n, n)
        let reader = try gpu.render(fragment: "read_texel_fragment", format: .rgba16Float)
        paths.append(
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(reader)
                var corners = GPU.fullScreen
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                e.setFragmentTexture(xr, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                e.endEncoding()
            }.path)
        let codes = try await gpu.read(xr, bytesPerPixel: 8)
        let reads = try await gpu.read(target, bytesPerPixel: 8)
        var codesWrong = 0, readsWrong = 0
        var first = "-"
        for i in 0..<(n * n) {
            // BGRA10_XR: four 16-bit fields, blue first, the code in each field's top 10 bits
            let field = { (f: Int) in Int(codes[8 * i + 2 * f]) | Int(codes[8 * i + 2 * f + 1]) << 8 }
            let want = (0..<4).map { Self.code(values[i][$0], alpha: $0 == 3) }
            let got = [field(2) >> 6, field(1) >> 6, field(0) >> 6, field(3) >> 6]
            if got != want {
                codesWrong += 1
                if first == "-" { first = "\(i): \(got) want \(want)" }
            }
            for k in 0..<4 {
                let bits = UInt16(reads[8 * i + 2 * k]) | UInt16(reads[8 * i + 2 * k + 1]) << 8
                let read = Float16(bitPattern: bits)
                readsWrong += read == Float16(Double(want[k] - 384) / 510) ? 0 : 1
            }
        }
        var metrics = Metrics()
        metrics.set("paths", paths.joined(separator: ","))
        metrics.set("codes_wrong", codesWrong)
        metrics.set("reads_wrong", readsWrong)
        metrics.set("first_wrong", first)
        var failures: [String] = []
        if paths.contains(where: { $0 != "raw" }) { failures.append("a command buffer off the raw path") }
        if codesWrong != 0 { failures.append("\(codesWrong) texels' codes wrong") }
        if readsWrong != 0 { failures.append("\(readsWrong) channels read wrong") }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}

/// A fragment function's storage write into a BGRA8Unorm texture over an IOSurface whose rows are 48 bytes apart: a
/// texture header's pitch takes multiples of 32, so the raw path samples it through a block-linear shadow (before this the
/// draw was refused). The draw writes the shadow, which goes back into the surface after the pass. Each byte of the
/// surface read on the CPU is the value written (codes k / 255, stored exactly), the padding past each row untouched.
/// PASS: no byte off, the command buffer on the raw path.
struct WrittenShadowedSurfaceTest: HWTest {
    let id = "gpu.raw.written_shadowed_surface"
    let name = "A fragment function's storage write into a surface of 48-byte rows, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let width = 8, height = 4, rowBytes = 48, padding: UInt8 = 0xA5

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let (w, h) = (Self.width, Self.height)
        guard
            let surface = IOSurfaceCreate(
                [
                    kIOSurfaceWidth as String: w, kIOSurfaceHeight as String: h,
                    kIOSurfaceBytesPerElement as String: 4, kIOSurfaceBytesPerRow as String: Self.rowBytes,
                    kIOSurfacePixelFormat as String: 0x4247_5241,
                ] as CFDictionary)
        else { throw TestFailure("no IOSurface") }
        IOSurfaceLock(surface, [], nil)
        memset(IOSurfaceGetBaseAddress(surface), Int32(Self.padding), Self.rowBytes * h)
        IOSurfaceUnlock(surface, [], nil)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
        descriptor.usage = [.shaderRead, .shaderWrite]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no texture over the surface")
        }
        let codes = (0..<(w * h)).map { i in (0..<4).map { UInt8((i * 29 + $0 * 71 + 3) % 256) } }
        let values = codes.map { SIMD4<Float>($0.map { Float($0) / 255 }) }
        let path = try await drawWrittenHalves(gpu, texture, values, width: w)
        IOSurfaceLock(surface, .readOnly, nil)
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        var wrong = 0, paddingWrong = 0
        for y in 0..<h {
            for x in 0..<w {
                let c = codes[y * w + x]
                let at = y * Self.rowBytes + 4 * x
                // BGRA8Unorm: blue first
                wrong += [base[at], base[at + 1], base[at + 2], base[at + 3]] == [c[2], c[1], c[0], c[3]] ? 0 : 1
            }
            for at in (y * Self.rowBytes + 4 * w)..<((y + 1) * Self.rowBytes) {
                paddingWrong += base[at] == Self.padding ? 0 : 1
            }
        }
        let firstTexel = "\(base[0])/\(base[1])/\(base[2])/\(base[3])"
        IOSurfaceUnlock(surface, .readOnly, nil)
        var metrics = Metrics()
        metrics.set("path", path)
        metrics.set("texels_wrong", wrong)
        metrics.set("padding_wrong", paddingWrong)
        metrics.set("texel0", firstTexel)
        var failures: [String] = []
        if path != "raw" { failures.append("the command buffer went off the raw path (\(path))") }
        if wrong != 0 { failures.append("\(wrong) texels wrong") }
        if paddingWrong != 0 { failures.append("\(paddingWrong) padding bytes changed") }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}

/// A fragment function's storage write into an RGB10A8_2P_XR10 texture over a 'b3a8' surface, which the raw path holds
/// in Metal's two planes (no image of its texels; such a texture with write usage was m2v's): the draw stores through an
/// RG32Uint image merged from the planes before its pass and split back into them after. Every texel of both planes, read
/// on the CPU, is Metal's: plane 0 the code word of the half written (XRTwoPlaneTest.code), plane 1 the alpha byte. PASS:
/// no texel off, the command buffer on the raw path.
struct WrittenXrPlanesTest: HWTest {
    let id = "gpu.raw.written_xr_planes"
    let name = "A fragment function's storage write into a two-plane extended-range surface, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let width = 16, height = 8

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let (w, h) = (Self.width, Self.height)
        var made: CVPixelBuffer?
        let attributes: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary]
        guard
            CVPixelBufferCreate(
                nil, w, h, kCVPixelFormatType_30RGBLE_8A_BiPlanar, attributes as CFDictionary, &made)
                == kCVReturnSuccess, let buffer = made,
            let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
        else { throw TestFailure("no b3a8 pixel buffer") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: XRTwoPlaneTest.format, width: w, height: h, mipmapped: false)
        descriptor.usage = [.shaderRead, .shaderWrite]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no 550 texture over the b3a8 surface")
        }
        let value = { (i: Int, k: Int) -> Float in
            k == 3 ? Float((i * 17) % 101) / 100 : -0.7 + 1.9 * Float(((i * 4 + k) * 31) % 97) / 96
        }
        let values: [SIMD4<Float>] = (0..<(w * h)).map { (i: Int) -> SIMD4<Float> in
            SIMD4<Float>(value(i, 0), value(i, 1), value(i, 2), value(i, 3))
        }
        let path = try await drawWrittenHalves(gpu, texture, values, width: w)
        let planes = XRTwoPlaneTest.planes(buffer, width: w, height: h)
        var wrong = 0
        var first = "-"
        for i in 0..<(w * h) {
            let v = values[i]
            let held = { (x: Float) -> Float in Float(Float16(x)) }
            let halves = SIMD4<Float>(held(v.x), held(v.y), held(v.z), held(v.w))
            let want = XRTwoPlaneTest.texel(halves)
            if planes[i] != want {
                wrong += 1
                if first == "-" { first = "\(i): \(String(planes[i], radix: 16)) want \(String(want, radix: 16))" }
            }
        }
        var metrics = Metrics()
        metrics.set("path", path)
        metrics.set("texels_wrong", wrong)
        metrics.set("first_wrong", first)
        var failures: [String] = []
        if path != "raw" { failures.append("the command buffer went off the raw path (\(path))") }
        if wrong != 0 { failures.append("\(wrong) texels wrong") }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}
