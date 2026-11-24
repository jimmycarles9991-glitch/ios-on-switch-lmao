import Foundation
import HWTestCore
import HWTestVideo

/// VTPixelTransferSession over each pair (video/PixelTransferRoutes.swift), five transfers a pair. Judged:
/// status_<pair>, each transfer's OSStatus, 0 for all eight pairs on an M4 Max; err_l008_bgra, max |R - L|,
/// |G - L|, |B - L| of L008 -> BGRA, 1 on an M4 Max (at most 1). Reported: route_<pair>, the session's
/// MostRecentConversionType, which both this Mac's VideoToolbox and the device's answer kVTPropertyNotSupportedErr
/// ("unsupported"), so no route is measured on either; and chain_<pair>, the session's MostRecentChainDescription,
/// where a pair VideoToolbox registers the scaler (VIC) for in 21H461 and 24B119 (420f 601 -> BGRA, 420v 709 -> BGRA,
/// BGRA sRGB/709 -> BGRA at 1:1 and half, L008 -> BGRA) reports a route other than hardware; and vic_maps_<pair>, the
/// SMMU mappings VIC made over the pair's transfers (kern.switch_vic_maps, debug kernels; every process's), which says
/// whether VideoToolbox sent the pair to the scaler at all.
struct PixelTransferRouteTest: HWTest {
    let id = "image.pixel_transfer_route"
    let name = "VTPixelTransferSession converts every pair, L008 to BGRA within a code of Apple's"
    let subsystem = Subsystem.images
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for pair in PixelTransferRoutes.pairs {
            do {
                let mapsBefore = Self.vicMaps()
                let r = try PixelTransferRoutes.run(pair)
                if let before = mapsBefore, let after = Self.vicMaps() {
                    metrics.set("vic_maps_" + pair.key, after - before)
                }
                metrics.set("route_" + pair.key, r.route)
                metrics.set("status_" + pair.key, Int(r.status))
                checks.append(("status_\(pair.key)=\(r.status)", r.status == 0))
                if pair.expected == "hardware" && r.route != "hardware" { metrics.set("chain_" + pair.key, r.chain) }
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

    static func vicMaps() -> Int? {
        guard case .value(let maps) = KernelSysctl.integer("kern.switch_vic_maps", as: Int32.self) else { return nil }
        return Int(maps)
    }
}
