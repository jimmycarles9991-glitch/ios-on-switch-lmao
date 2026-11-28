import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import IOSurface
import Metal

/// The copy engine skips a launch whose guard word is 0 (TegraGM20BCopy.h tegra_gm20b_ce_wait.predicated), on the
/// device: what keeps a cancelled JPEG decode's re-pack from copying the service's shared buffer, which may hold another
/// process's picture, into the caller's surface. A BGRA surface the GPU clears to one colour (block-linear, so NVJPG's
/// decode goes by the copy engine into its blocks), then a debug kernel's copy-guard probe (kGuardProbeMagic, 0x5047)
/// decodes into it with the re-pack's guard closed: the surface must still read the clear colour
/// (guard_closed_changed= bytes not the clear's, 0). Then an ordinary decode must land the picture
/// (guard_open_wrong= bytes past JPEGSurfaceDecode.tolerance, 0), both by the copy engine (routes=).
struct NVJPGCopyGuardTest: HWTest {
    let id = "image.nvjpg_copy_guard"
    let name = "A copy-engine re-pack whose guard is 0 leaves the destination as it was"
    let subsystem = Subsystem.images
    var timeout: TimeInterval { 30 }

    static let size = NVJPGTileProbeTest.size
    static let clear: [UInt8] = [0x33, 0x66, 0x99, 0xff] // BGRA bytes of MTLClearColor(0.6, 0.4, 0.2, 1)

    func run(_ context: TestContext) async throws -> Outcome {
        guard case .success = JPEGRoute.decodes() else { return .skip("no route counts: a release kernel") }
        guard let file = JPEGSurfaceDecode.file(Self.size) else { throw TestFailure("no JPEG of \(Self.size)") }
        let gpu = try GPU.make()
        let (surface, target) = try await Self.cleared(gpu)
        let first = try? JPEGRoute.decodes().get()
        let closed = JPEGSurfaceDecode.decode(file, into: surface, magic: 0x5047)
        let changed = Self.changed(SurfaceCrossProcessTest.cpuRows(surface).0)
        let open = JPEGSurfaceDecode.decode(file, into: surface)
        let routes = NVJPGTileProbeTest.routes(from: first, to: try? JPEGRoute.decodes().get())
        let (wrong, _) = JPEGSurfaceDecode.wrong(SurfaceCrossProcessTest.cpuRows(surface).0, surface)
        withExtendedLifetime(target) { _ = $0 }
        var metrics = Metrics()
        metrics.set("guard_closed_decode", closed?.description ?? "ok")
        metrics.set("guard_closed_changed", changed)
        metrics.set("guard_open_decode", open?.description ?? "ok")
        metrics.set("guard_open_wrong", wrong)
        metrics.set("routes", routes)
        let checks = [
            ("both decodes go by the copy engine into the blocks (\(routes))", routes == "copy_blocks:2"),
            ("a closed guard leaves the surface as it was (\(changed) bytes changed)", closed == nil && changed == 0),
            ("an open guard lands the picture (\(wrong) bytes differ)", open == nil && wrong == 0),
        ]
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// Bytes of a CPU read that are not the clear colour's
    static func changed(_ rows: [UInt8]) -> Int {
        rows.indices.filter { rows[$0] != clear[$0 % 4] }.count
    }

    /// A BGRA surface decided uncompressed block-linear first, then cleared on the GPU
    static func cleared(_ gpu: GPU) async throws -> (IOSurface, MTLTexture) {
        let (surface, sampler) = try await NVJPGTileProbeTest.blockLinear(gpu)
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: size, height: size, mipmapped: false
        )
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .shared
        guard
            let texture = gpu.device.makeTexture(
                descriptor: d, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0
            )
        else { throw TestFailure("no texture over the surface") }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.6, green: 0.4, blue: 0.2, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        withExtendedLifetime(sampler) { _ = $0 }
        return (surface, texture)
    }
}
