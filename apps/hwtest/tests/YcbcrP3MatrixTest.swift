import CoreVideo
import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// Two-plane YCbCr textures over surfaces tagged kIOSurfaceYCbCrMatrix P3_D65 or DCI_P3. An M4 Max converts nothing
/// under those matrices (harness/ycbcroracle.mm): a converted format reads its codes as red Y, green Cb, blue Cr,
/// which is its surface's codes format's read (red Cr, green Y, blue Cb) turned. Each arm reads both textures over one
/// surface of random codes, each texture read by a draw: off_<format>_<matrix> counts the converted texels that are not
/// the codes texels turned (0 to pass), paths the command buffers' paths (raw/N to pass).
struct YcbcrP3MatrixTest: HWTest {
    let id = "gpu.gm20b.ycbcr_p3"
    let name = "YCbCr textures under the P3 matrices read their codes, as Metal reads them, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Arm {
        let converted: UInt
        let codes: UInt
        let fourcc: OSType
    }

    static let arms = [
        Arm(converted: 500, codes: 540, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        Arm(converted: 503, codes: 542, fourcc: kCVPixelFormatType_444YpCbCr8BiPlanarFullRange),
        Arm(converted: 505, codes: 543, fourcc: kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for matrix in ["P3_D65", "DCI_P3"] {
            for arm in Self.arms {
                let key = "\(arm.converted)_\(matrix == "P3_D65" ? "p3" : "dci")"
                let off = try await armOff(gpu, arm, matrix)
                metrics.set("off_" + key, off.map(String.init) ?? "no texture")
                checks.append(("\(key): the converted texture reads the codes turned", off == 0))
            }
        }
        let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "gm20b" }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        checks.append(("every command buffer raw", raw))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// The converted texels off the codes texels turned; nil where a texture was not made
    func armOff(_ gpu: GPU, _ arm: Arm, _ matrix: String) async throws -> Int? {
        let (w, h) = (34, 18)
        guard let buffer = YcbcrCornerTest.pixelBuffer(w, h, arm.fourcc),
              let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
        else { return nil }
        CVPixelBufferLockBaseAddress(buffer, [])
        var seed: UInt32 = 0x9e37_79b9 &* UInt32(arm.converted)
        for plane in 0 ..< CVPixelBufferGetPlaneCount(buffer) {
            guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { continue }
            let bytes = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane) * CVPixelBufferGetHeightOfPlane(buffer, plane)
            let p = base.bindMemory(to: UInt8.self, capacity: bytes)
            for i in 0 ..< bytes {
                seed = seed &* 1_664_525 &+ 1_013_904_223
                p[i] = UInt8(truncatingIfNeeded: seed >> 24)
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, [])
        IOSurfaceSetValue(surface, "IOSurfaceYCbCrMatrix" as CFString, matrix as CFString)
        let make = { (format: UInt) -> MTLTexture? in
            guard let pixelFormat = MTLPixelFormat(rawValue: format) else { return nil }
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: pixelFormat, width: w, height: h, mipmapped: false
            )
            d.usage = .shaderRead
            return gpu.device.makeTexture(descriptor: d, iosurface: surface, plane: 0)
        }
        guard let converted = make(arm.converted), let codes = make(arm.codes) else { return nil }
        let a = try await Self.drawnTexels(gpu, converted), c = try await Self.drawnTexels(gpu, codes)
        return zip(a, c).filter { $0.0 != SIMD4<Float>($0.1.y, $0.1.z, $0.1.x, $0.1.w) }.count
    }

    /// A texture's texels as a draw reads them into an RGBA32Float target (a raw kernel samples no converted image)
    static func drawnTexels(_ gpu: GPU, _ texture: MTLTexture) async throws -> [SIMD4<Float>] {
        let (w, h) = (texture.width, texture.height)
        let state = try gpu.render(fragment: "texel_read", format: .rgba32Float)
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
            encoder.setFragmentTexture(texture, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let bytes = try await gpu.read(target, bytesPerPixel: 16)
        return bytes.withUnsafeBytes { Array($0.bindMemory(to: SIMD4<Float>.self).prefix(w * h)) }
    }
}
