import Foundation

/// gpu.gm20b.standin_ms_resolve's reads of one case's stored samples: each against a one-sample draw of its value, that
/// texture read by a compute kernel (`computeOff`) and by a fragment pass (`fragmentOff`), and the texels where the two
/// reads of it differ (`routesDiffer`). Where the two reads differ, a readback is broken, so both must agree for every
/// format. A compute read the driver refuses ends the compute side (`computeRefusal`); the fragment side reads every
/// sample regardless. `firstDiffer`: the first texel the two read differently, its bytes in the texture (hex, as the
/// CPU reads them) and both reads' red, alpha and stored sample's red, so a Mac can tell which route decodes the code
/// as Metal does.
public struct SampleRouteReadings: Sendable, Equatable {
    public var computeOff = 0, fragmentOff = 0, routesDiffer = 0
    public var computeRefusal: String?
    public var firstDiffer = "-"

    public init() {}

    /// `<key>_samples_off_fragment`, then `<key>_samples_off`, `<key>_routes_differ` and `<key>_first_differ`, or
    /// `<key>_compute_refused=1`
    public func record(_ key: String, _ metrics: inout Metrics) {
        metrics.set(key + "_samples_off_fragment", fragmentOff)
        guard computeRefusal == nil else { return metrics.set(key + "_compute_refused", 1) }
        metrics.set(key + "_samples_off", computeOff)
        metrics.set(key + "_routes_differ", routesDiffer)
        metrics.set(key + "_first_differ", firstDiffer)
    }

    public func checks(_ key: String) -> [(String, Bool)] {
        let fragment = (
            "\(key): every sample stores its one-sample code, as a fragment pass reads it", fragmentOff == 0
        )
        if let computeRefusal {
            return [fragment, ("\(key) samples through a compute kernel: \(computeRefusal)", false)]
        }
        return [
            ("\(key): every sample stores its one-sample code", computeOff == 0), fragment,
            ("\(key): a compute kernel and a fragment pass read each one-sample texture alike", routesDiffer == 0),
        ]
    }
}
