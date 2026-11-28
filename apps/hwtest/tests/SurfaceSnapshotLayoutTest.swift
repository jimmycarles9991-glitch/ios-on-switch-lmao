import Darwin
import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface

/// Core Animation's snapshots as boot 22 made them (522 of them, 1280x720 and 720x1280 BGRA, all pitch): one process
/// makes the IOSurface, which maps it for its CPU, the render server renders it first, and the maker reads it under
/// IOSurfaceLock. Here the app makes four 1280x720 BGRA surfaces with Core Animation's 5120-byte rows, the texture peer
/// (peer/texpeer.mm) renders each first and keeps its texture, as the render server's compositor target does, and the
/// app reads each through a read-only lock. A CPU mapping in another process is no pitch reason
/// (drivers/TegraGM20B/TegraGM20BTiledDecision.h): every surface is block-linear and every lock reads the peer's rows.
/// Fields: `blocklinear` and `pitch` (how many of the four), `layouts`, `cpu_wrong` (bytes off the peer's pattern over
/// all four), `gob` (lock reads laid out in GOBs), `read_us_max`.
struct SurfaceSnapshotLayoutTest: HWTest {
    let id = "gpu.iosurface.snapshot_layout"
    let name = "An IOSurface one process makes and another renders first is block-linear and reads as rows under a lock"
    let subsystem = Subsystem.metal

    static let surfaces = 4
    static let width = 1280, height = 720, rowBytes = 5120

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else { return .skip("the default device is not the facade") }
        guard let kernel = GPUConnection() else { return .skip("no TegraGM20B connection") }
        let connection = try await TexturePeerLink.connection()
        var blockLinear = 0, pitch = 0, cpuWrong = 0, gob = 0, readMax = 0, rendered = 0
        var layouts: [String] = []
        var held: [IOSurface] = []
        for _ in 0 ..< Self.surfaces {
            let properties: [IOSurfacePropertyKey: Any] = [
                .width: Self.width, .height: Self.height, .bytesPerElement: 4, .bytesPerRow: Self.rowBytes,
                .allocSize: Self.rowBytes * Self.height, .pixelFormat: UInt32(0x4247_5241),
            ]
            guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface not made") }
            held.append(surface)
            let answer = await SurfaceCrossProcessTest.ask(connection) {
                $0.renderSurface(surface, keep: true, reply: $1)
            }
            guard answer?.hasPrefix("ok ") == true else {
                layouts.append("unrendered")
                continue
            }
            rendered += 1
            let (kr, layout) = kernel.surfaceLayout(surface)
            layouts.append(kr == KERN_SUCCESS ? String(layout) : GPUConnection.hex(kr))
            if kr == KERN_SUCCESS && layout == 1 { blockLinear += 1 } else if kr == KERN_SUCCESS { pitch += 1 }
            let (rows, readUs) = SurfaceCrossProcessTest.cpuRows(surface)
            readMax = max(readMax, readUs)
            cpuWrong += SurfaceCrossProcessTest.wrong(rows, surface).0
            gob += SurfaceCrossProcessTest.readAsGobs(rows, surface) ? 1 : 0
        }
        _ = await SurfaceCrossProcessTest.ask(connection) { $0.dropSurfaces($1) }
        withExtendedLifetime(held) {}
        var metrics = Metrics()
        metrics.set("rendered", rendered)
        metrics.set("blocklinear", blockLinear)
        metrics.set("pitch", pitch)
        metrics.set("layouts", layouts.joined(separator: "/"))
        metrics.set("cpu_wrong", cpuWrong)
        metrics.set("gob", gob)
        metrics.set("read_us_max", readMax)
        let checks = [
            await TexturePeerLink.startedOnce(&metrics),
            ("the peer rendered all \(Self.surfaces) surfaces", rendered == Self.surfaces),
            ("all \(Self.surfaces) block-linear (\(blockLinear), pitch \(pitch))", blockLinear == Self.surfaces),
            ("every lock read is the peer's rows (\(cpuWrong) bytes differ)", rendered > 0 && cpuWrong == 0),
        ]
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
