import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import IOSurface
import Metal

/// NVJPG into a BGRA surface GM20B laid out block-linear (a GPU sampler asked first), 256x256 (16-GOB blocks), on a
/// debug kernel. route: the decode takes the copy engine into the blocks (TegraNVJPGRoute.h: the T210's NVJPG writes
/// rows), route_taken= the route counts' change, and the CPU read is the picture (route_wrong= bytes past
/// JPEGSurfaceDecode.tolerance). The tile probes, reported and not judged: NVJPG told tile_mode 2 then 1 and block
/// height codes 2 to 4 (16-GOB blocks are code 4, or code 3 if the decoder codes as the encoder does) writes the
/// destination's blocks itself; m<mode>_g<code>= the layout its bytes match
/// (JPEGSurfaceDecode.explained: rows, y<n> GM20B's Tegra GOBs, fermi_y<n> NIL's Fermi GOBs, - none, exact when
/// m<mode>_g<code>_wrong=0), which settles whether any tiling setup writes GM20B's layout natively, or `cancelled`: the
/// kernel cancelled the job at its bound and reset the engine (its "job fence ... cancelled" line names the tiling).
/// Judged: NVJPG decodes a picture by rows before the probes and after each one, so a probe that left the engine
/// unusable is named (stopped_after=) and no probe runs after it. Runs after every other image test.
struct NVJPGTileProbeTest: HWTest {
    let id = "image.nvjpg_tile_probe"
    let name = "NVJPG into a block-linear surface goes through the copy engine; what each tiling setup writes"
    let subsystem = Subsystem.images
    var timeout: TimeInterval { 60 }

    static let size = 256
    /// tile_mode 2 has run on the device without a hang (25 Nov, codes 4 and 0); tile_mode 1 never has
    static let probes: [(mode: UInt32, gob: UInt32)] = [(2, 2), (2, 3), (2, 4), (1, 2), (1, 3), (1, 4)]

    func run(_ context: TestContext) async throws -> Outcome {
        guard case .success = JPEGRoute.decodes() else {
            return .skip("no route counts: a release kernel")
        }
        guard let file = JPEGSurfaceDecode.file(Self.size) else { throw TestFailure("no JPEG of \(Self.size)") }
        let gpu = try GPU.make()
        var metrics = Metrics()
        var checks: [(String, Bool)] = []

        let rows = try Self.rowsDecode(file)
        metrics.set("rows_before", rows.description)
        checks.append(("NVJPG decodes the picture by rows before the probes (\(rows))", rows.right))
        if !rows.right {
            metrics.set("stopped_after", "rows_before")
            return .judged(checks, metrics, proof: gpu.proof)
        }

        let (surface, sampler) = try await Self.blockLinear(gpu)
        let first = try? JPEGRoute.decodes().get()
        let failure = JPEGSurfaceDecode.decode(file, into: surface)
        let taken = Self.routes(from: first, to: try? JPEGRoute.decodes().get())
        let (wrong, _) = JPEGSurfaceDecode.wrong(SurfaceCrossProcessTest.cpuRows(surface).0, surface)
        metrics.set("route_decode", failure?.description ?? "ok")
        metrics.set("route_taken", taken)
        metrics.set("route_wrong", wrong)
        checks.append(
            ("the decode goes through the copy engine into the blocks (\(taken))", taken == "copy_blocks:1")
        )
        checks.append(("the CPU reads the picture (\(wrong) bytes differ)", failure == nil && wrong == 0))
        withExtendedLifetime(sampler) { _ = $0 }

        var cancelled = 0
        for probe in Self.probes {
            let key = "m\(probe.mode)_g\(probe.gob)"
            let (target, held) = try await Self.blockLinear(gpu)
            let before = try? JPEGRoute.decodes().get()
            let failure = JPEGSurfaceDecode.decode(file, into: target, tile: probe)
            let after = try? JPEGRoute.decodes().get()
            if let before, let after, after.cancelled != before.cancelled {
                cancelled += 1
                metrics.set(key, "cancelled")
            } else if let failure {
                metrics.set(key, "failed:" + String(failure.description.prefix(40)))
            } else {
                let got = SurfaceCrossProcessTest.cpuRows(target).0
                metrics.set(key, JPEGSurfaceDecode.explained(got, target))
                metrics.set(key + "_wrong", JPEGSurfaceDecode.wrong(got, target).0)
            }
            withExtendedLifetime(held) { _ = $0 }
            let next = try Self.rowsDecode(file)
            checks.append(("after \(key) NVJPG decodes the picture by rows (\(next))", next.right))
            if !next.right {
                metrics.set("stopped_after", key)
                break
            }
        }
        metrics.set("cancelled", cancelled)
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// A decode into a surface no GPU sampler has asked for, which NVJPG writes as rows
    struct RowsDecode: CustomStringConvertible {
        let failure: TestFailure?
        let route: String
        let wrong: Int

        var right: Bool { failure == nil && route == "rows:1" && wrong == 0 }
        var description: String {
            failure.map { "failed:" + String($0.description.prefix(40)) } ?? "\(route), \(wrong) bytes differ"
        }
    }

    static func rowsDecode(_ file: Data) throws -> RowsDecode {
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface \(size) not made") }
        let before = try? JPEGRoute.decodes().get()
        let failure = JPEGSurfaceDecode.decode(file, into: surface)
        let after = try? JPEGRoute.decodes().get()
        let route = routes(from: before, to: after)
        let wrong = JPEGSurfaceDecode.wrong(SurfaceCrossProcessTest.cpuRows(surface).0, surface).0
        return RowsDecode(failure: failure, route: route, wrong: wrong)
    }

    static func routes(from first: JPEGRoute.Counts?, to last: JPEGRoute.Counts?) -> String {
        guard let first, let last else { return "-" }
        return JPEGRoute.routes(from: first, to: last)
    }

    static var properties: [IOSurfacePropertyKey: Any] {
        [.width: size, .height: size, .bytesPerElement: 4, .pixelFormat: UInt32(0x4247_5241)]
    }

    /// A BGRA surface the GPU samples first, so GM20B lays it out block-linear, and the texture that sampled it (held
    /// until the reads, as gpu.iosurface.cross_process holds its sampler)
    static func blockLinear(_ gpu: GPU) async throws -> (IOSurface, MTLTexture) {
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface \(size) not made") }
        return (surface, try await SurfaceCrossProcessTest.gpuRows(gpu, surface).1)
    }
}
