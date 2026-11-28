import CoreVideo
import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Passes into two-plane YCbCr surface textures made without RenderTarget usage: 32x32 surfaces '420v' (formats 540,
/// 500 and 520), '422v' (502), '444v' (503) and 'x420' (505), both planes scribbled, a texture of each made with
/// ShaderRead (r) or ShaderRead and ShaderWrite (rw) usage, a pass Clear (0.5, 0.25, 0.75, 1), Store. Both planes must
/// equal, byte for byte, those of the same pass into a twin surface's texture of RenderTarget and ShaderRead usage (an
/// M4 Max: m2v-passes3 probe/ycbcrnort.mm, out4.txt, 0 bytes differ in all 12; this scene on an M4 Max: the same).
/// Expected to fail on the device until the driver's texture side gives every usage the stand-in such a pass needs:
/// until then the command buffer fails Internal (enc.planarrt). Per format and usage <fmt>_<usage>_differ bytes that
/// differ; every command buffer raw, failed_cbs=0.
struct YcbcrNoRenderUsageTest: HWTest {
    let id = "gpu.gm20b.ycbcr_norender_usage"
    let name =
        "Passes into YCbCr surface textures without RenderTarget usage store what a RenderTarget one does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let side = 32
    static let cases: [(format: UInt, fourcc: OSType)] = [
        (540, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange), (500, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (520, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange), (502, kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange),
        (503, kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange), (505, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange),
    ]
    static let usages: [(key: String, usage: MTLTextureUsage)] = [
        ("r", [.shaderRead]), ("rw", [.shaderRead, .shaderWrite]),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for c in Self.cases {
            for u in Self.usages {
                let key = "\(c.format)_\(u.key)"
                do {
                    let differ = try await Self.differ(gpu, c.format, c.fourcc, u.usage)
                    metrics.set(key + "_differ", differ)
                    checks.append(("\(key): both planes those of a RenderTarget texture's pass", differ == 0))
                } catch {
                    checks.append(("\(key): \(GPU.describe(error))", false))
                }
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: ["paths", "failed_cbs"])
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func planes(_ buffer: CVPixelBuffer) -> [UInt8] {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        return (0 ..< CVPixelBufferGetPlaneCount(buffer)).flatMap { plane -> [UInt8] in
            guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { return [] }
            let count = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane) * CVPixelBufferGetHeightOfPlane(buffer, plane)
            return [UInt8](UnsafeRawBufferPointer(start: base, count: count))
        }
    }

    static func cleared(_ gpu: GPU, _ format: UInt, _ fourcc: OSType, _ usage: MTLTextureUsage) async throws -> [UInt8]
    {
        guard let buffer = YcbcrCornerTest.pixelBuffer(side, side, fourcc),
              let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
              let pixelFormat = MTLPixelFormat(rawValue: format)
        else { throw TestFailure("no \(format) surface") }
        YcbcrTargetTest.scribble(buffer, seed: 0)
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: pixelFormat, width: side, height: side, mipmapped: false
        )
        d.usage = usage
        guard let texture = gpu.device.makeTexture(descriptor: d, iosurface: surface, plane: 0) else {
            throw TestFailure("no \(format) texture of usage \(usage.rawValue)")
        }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.5, green: 0.25, blue: 0.75, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        return planes(buffer)
    }

    static func differ(_ gpu: GPU, _ format: UInt, _ fourcc: OSType, _ usage: MTLTextureUsage) async throws -> Int {
        let got = try await cleared(gpu, format, fourcc, usage)
        let want = try await cleared(gpu, format, fourcc, [.renderTarget, .shaderRead])
        guard got.count == want.count else { return max(got.count, want.count) }
        return zip(got, want).filter { $0 != $1 }.count
    }
}
