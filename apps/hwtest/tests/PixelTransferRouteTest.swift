import Foundation
import HWTestCore
import HWTestVideo

/// The unit VTPixelTransferSession takes for each pair (video/PixelTransferRoutes.swift), five transfers a pair, read off
/// the session's MostRecentConversionType: route_<pair> hardware for the pairs VideoToolbox registers the scaler (VIC)
/// for in 21H461 and 24B119 (420f 601 -> BGRA, 420v 709 -> BGRA, BGRA sRGB/709 -> BGRA at 1:1 and half, L008 -> BGRA),
/// metal for the rest (BGRA -> L008, a colour-space change, the 10-bit packed pairs), which are reported. status_<pair>:
/// each transfer's OSStatus, 0 for all eight pairs on an M4 Max. err_l008_bgra: max |R - L|, |G - L|, |B - L| of
/// L008 -> BGRA, 1 on an M4 Max (judged at most 1). This Mac's VideoToolbox answers the property itself
/// kVTPropertyNotSupportedErr (route "unsupported"), so no route is an Apple measurement; one the device does not
/// answer fails. chain: the session's MostRecentChainDescription where a hardware pair took another route.
struct PixelTransferRouteTest: HWTest {
    let id = "image.pixel_transfer_route"
    let name = "VTPixelTransferSession takes the scaler for the pairs VideoToolbox registers it for"
    let subsystem = Subsystem.images
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for pair in PixelTransferRoutes.pairs {
            do {
                let r = try PixelTransferRoutes.run(pair)
                metrics.set("route_" + pair.key, r.route)
                metrics.set("status_" + pair.key, Int(r.status))
                checks.append(("status_\(pair.key)=\(r.status)", r.status == 0))
                if pair.expected == "hardware" {
                    checks.append(("route_\(pair.key)=\(r.route) not hardware", r.route == "hardware"))
                    if r.route != "hardware" { metrics.set("chain_" + pair.key, r.chain) }
                }
                if let error = r.grayError {
                    metrics.set("err_" + pair.key, error)
                    checks.append(("err_\(pair.key)=\(error)", error <= 1))
                }
            } catch {
                checks.append(("\(pair.key): \(error)", false))
            }
        }
        return .judged(checks, metrics)
    }
}
