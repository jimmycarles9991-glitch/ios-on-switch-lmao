import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The level of detail a minified mipmapped texture is sampled at, against Metal's formula (HWTestGPU MipLod): `lod=`
/// each level arm's red as `<arm>=<got>/<want>` (n<side>: nearest mip filter, l<side>: linear, b_*: a shader bias,
/// minclamp/maxclamp: the sampler's level clamps, notmip: no mipmapping, view: a view of levels 1 to 3); `icon_<side>_*`
/// a coverage-antialiased rounded rectangle through generateMipmaps and a trilinear sampler, drawn at 29 to 100 pixels,
/// against the CPU's reference of Metal's draw. FAIL: an arm beyond a twentieth of a level, or an icon draw with a code
/// beyond MipLod.iconTolerance.
struct MipLodTest: HWTest {
    let id = "gpu.raw.mip_lod"
    let name = "Minified mipmapped textures sample the level and blend Metal's level of detail gives"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        var metrics = Metrics()
        metrics.set("dev", RawFacade.kind(of: gpu.device))
        let levels = try await MipLod.levels(gpu)
        let icon = try await MipLod.icon(gpu)
        var wrong = levels.wrong.map { "lod_" + $0 }
        metrics.set("lod_wrong", levels.wrong.count)
        metrics.set("lod", levels.fields.joined(separator: ","))
        for cell in icon.cells {
            metrics.set("icon_\(cell.side)_beyond", cell.beyond)
            metrics.set("icon_\(cell.side)_max_diff", cell.maxDiff)
            if cell.beyond > 0 { wrong.append("icon_\(cell.side)") }
        }
        metrics.set("paths", levels.paths.union([icon.path]).sorted().joined(separator: ","))
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
