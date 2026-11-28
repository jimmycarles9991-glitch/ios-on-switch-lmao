import Foundation

/// How the driver keeps Metal's raster order for a state that reads the colour it writes, as the facade's fetch counters
/// (switchMetalCounters, debug builds) count its calls. A state with a storage form draws each call whole inside the
/// pixel interlock (fetch_interlocked); one without draws each call as one call of the fetch macro, a BEGIN a primitive
/// behind the fetch barrier on the GPU (fetch_sequential, and fetch_triangles the primitives times instances of its
/// direct draws). A call is a draw, an indirect draw or an indirect command buffer's execute; on either route the CPU
/// writes its words once whatever its primitive count, so the other route's counters stay where they were.
public enum FetchRoute: String, Sendable {
    case interlock
    case macro

    public static let keys = ["fetch_interlocked", "fetch_sequential", "fetch_triangles"]

    /// The counters' moves `calls` fetching calls of `triangles` primitives make on this route
    public func expected(calls: Int, triangles: Int) -> [String: Int] {
        switch self {
        case .interlock: return ["fetch_interlocked": calls, "fetch_sequential": 0, "fetch_triangles": 0]
        case .macro: return ["fetch_interlocked": 0, "fetch_sequential": calls, "fetch_triangles": triangles]
        }
    }

    /// One check a counter, each naming what it rose by and what this route wants
    public func checks(calls: Int, triangles: Int, moved: [String: Int]) -> [(String, Bool)] {
        let want = expected(calls: calls, triangles: triangles)
        return Self.keys.map { key in
            let rose = moved[key] ?? 0
            return ("\(rawValue): \(key) rose by \(rose), want \(want[key] ?? 0)", rose == want[key])
        }
    }
}

extension FetchRoute {
    /// A default fetching draw's route as rule fetch.twin names it (drivers/TegraGM20BMetal/twin_rule.h): `twins` draws
    /// its barrier twin took by the rule (fetch_twin_rule) and `interlocked` draws inside the pixel interlock
    /// (interlock_draws), one check a counter
    public static func ruleChecks(
        _ label: String, twins: Int, interlocked: Int, moved: [String: Int]
    ) -> [(String, Bool)] {
        let rule = moved["fetch_twin_rule"] ?? 0, interlocks = moved["interlock_draws"] ?? 0
        return [
            ("\(label): fetch_twin_rule rose by \(rule), want \(twins)", rule == twins),
            ("\(label): interlock_draws rose by \(interlocks), want \(interlocked)", interlocks == interlocked),
        ]
    }
}
