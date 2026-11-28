import Foundation
import HWTestCommon
import HWTestCore
import HWTestKit
import HWTestShared
import IOSurface

struct JPEGMappingLifetimeTest: HWTest {
    let id = "image.jpeg_mapping_lifetime"
    let name = "Repeated JPEG decodes retain one destination mapping"
    let subsystem = Subsystem.images

    func run(_ context: TestContext) async throws -> Outcome {
        let size = 256
        guard let file = JPEGSurfaceDecode.file(size),
              let surface = IOSurface(properties: [
                  .width: size, .height: size, .bytesPerElement: 4,
                  .bytesPerRow: size * 4, .pixelFormat: UInt32(0x4247_5241),
              ])
        else { return .fail("could not create JPEG or destination surface") }
        let first = try JPEGRoute.decodes().get()
        if let failure = JPEGSurfaceDecode.decode(file, into: surface) {
            return .fail(failure.description)
        }
        let warm = try JPEGRoute.decodes().get()
        let repeats = 4
        var wrong = 0
        for _ in 0 ..< repeats {
            if let failure = JPEGSurfaceDecode.decode(file, into: surface) {
                return .fail(failure.description)
            }
            let (pixels, _) = SurfaceCrossProcessTest.cpuRows(surface)
            wrong += JPEGSurfaceDecode.wrong(pixels, surface).0
        }
        let last = try JPEGRoute.decodes().get()
        let firstMaps = warm.outputMaps &- first.outputMaps
        let steadyMaps = last.outputMaps &- warm.outputMaps
        let decodes = last.decodes &- warm.decodes
        var metrics = Metrics()
        metrics.set("first_maps", Int(firstMaps))
        metrics.set("steady_maps", Int(steadyMaps))
        metrics.set("decodes", Int(decodes))
        metrics.set("wrong", wrong)
        return .judged(
            [
                ("first decode maps the destination once", firstMaps == 1),
                ("retained destination makes no new mapping", steadyMaps == 0),
                ("every repeated decode uses NVJPG", decodes == UInt64(repeats)),
                ("every destination read matches the JPEG", wrong == 0),
            ], metrics
        )
    }
}
