import Darwin
import HWTestCore
import HWTestShared

/// The kernel's media identity counts since boot (debug kernels): SMMU mappings VIC made and remade after evicting
/// them for room, window B's remakes, and native publications that needed a new surface object or went into a surface
/// with no native picture before. A pool that keeps one identity maps each buffer once, remakes none, and makes one
/// object a client surface.
struct MediaMapCounters {
    static let names = [
        "kern.switch_vic_maps", "kern.switch_vic_remaps", "kern.switch_native_objects", "kern.switch_native_surfaces",
    ]
    /// Window B's remakes: a display without it counts none (d_remaps is then left out).
    static let planeRemaps = "kern.switch_planes_remaps"

    let values: [String: Int]

    /// The counts, or the errno of the first that could not be read.
    static func read() -> (counters: MediaMapCounters?, unread: Int32) {
        var values: [String: Int] = [:]
        for name in names {
            switch KernelSysctl.integer(name, as: Int32.self) {
            case .value(let value): values[name] = Int(value)
            case .failed(let error): return (nil, error)
            }
        }
        if case .value(let value) = KernelSysctl.integer(planeRemaps, as: Int32.self) {
            values[planeRemaps] = Int(value)
        }
        return (MediaMapCounters(values: values), 0)
    }

    func since(_ before: MediaMapCounters, _ name: String) -> Int {
        (values[name] ?? 0) - (before.values[name] ?? 0)
    }

    /// vic_maps, vic_remaps, d_remaps, maps_steady (every remake), surface_objects and surfaces since `before`, and the
    /// checks they decide. `objects` judges one object a surface too: only where nothing but the CPU reads the
    /// pictures, since a GPU texture of a published picture holds its object past the surface's next publication.
    func judge(
        since before: MediaMapCounters, into metrics: inout Metrics, objects judgeObjects: Bool
    ) -> [(String, Bool)] {
        let remaps = since(before, "kern.switch_vic_remaps"), planeRemaps = since(before, Self.planeRemaps)
        let objects = since(before, "kern.switch_native_objects")
        let surfaces = since(before, "kern.switch_native_surfaces")
        metrics.set("vic_maps", since(before, "kern.switch_vic_maps"))
        metrics.set("vic_remaps", remaps)
        if values[Self.planeRemaps] != nil && before.values[Self.planeRemaps] != nil {
            metrics.set("d_remaps", planeRemaps)
        }
        metrics.set("maps_steady", remaps + planeRemaps)
        metrics.set("surface_objects", objects)
        metrics.set("surfaces", surfaces)
        var checks = [("no buffer mapped again after eviction (maps_steady 0)", remaps + planeRemaps == 0)]
        if judgeObjects {
            checks.append(
                ("one publication object a client surface (surface_objects <= surfaces)", objects <= surfaces)
            )
        }
        return checks
    }
}
