import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// gpu.x1.rotated_target (METALMAP d26): passes into a texture Metal's rotation SPI turns over an IOSurface plane (a
/// CAMetalLayer drawable on the turned display), rotations 1 to 4, against the same draws into a plain texture turned
/// by Metal's measured store map. The driver draws straight into the plane through the viewport coordinate swizzle, a
/// state that reads the plane's axes through its turned variant (its position, one axis's derivative), a private depth
/// texture neither loaded nor stored through a backing laid out as the plane, a 4x resolve into the plane through the
/// resolve state's turned variant, a private colour texture stored beside the plane, a private depth texture loaded
/// beside it and a private depth/stencil texture stored then loaded beside it (each through a stand-in laid out as the
/// plane, turned in and back by the pack's turn copies and stencil bits), rotation 4's load, which Metal makes
/// transposed in each tile a draw reaches (its expected bytes the reference pass loading pass 1's frame flipped top to
/// bottom, every tile reached by a full-frame draw whose fragments are discarded), loads whose draws reach some tiles
/// alone (RotatedReachCells.swift: rotation 4's plane and a second plane of rotation 4 beside the target), a load with
/// no draw, which leaves the plane as it is, a second plane of another turn and frame beside it and private textures of
/// other sizes (RotatedMixedCells.swift: each landing by its own store map over the render area, a turned attachment of
/// a frame past the area loaded as Metal loads it): those cells FAIL when they run anywhere else (path= other than
/// raw). native_cells / native_raw count the required cells and those that ran raw; tie_wrong counts the pixels the
/// plane's rasteriser decides apart from the frame's where an edge runs through pixel centres (its tie rule turns too);
/// r4_edge_wrong those of a rotation-4 load whose rectangle ends on tile edges, which Metal's reach takes past them.
struct RotatedTargetTest: HWTest {
    let id = "gpu.x1.rotated_target"
    let name = "Rotated drawables: rendered straight into the turned plane, each rotation"
    var subsystem: Subsystem { .metal }
    var timeout: TimeInterval { 240 }

    /// A frame over a plane whose rows are 256-byte multiples (a direct pitch target), one over rows of the plane's
    /// width (a pitch the render hardware takes only through a shadow, as the device's 60x60 and 720x1280 drawables'
    /// were), and the device's 1280x720 drawable for its clear and a load that reaches some tiles
    static let shapes: [(name: String, width: Int, height: Int, aligned: Bool, cells: [RotatedCell])] = [
        ("a", 96, 64, true, RotatedCell.targetCells), ("n", 90, 60, false, RotatedCell.targetCells),
        ("hd", 1280, 720, false, [.clear, .partLoad]),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let raw = GM20BFacade.kind(of: gpu.device) == "gm20b"
        let heapFullBefore = GM20BFacade.counters(of: gpu.device)["heap_full"] ?? 0
        let states = try RotatedStates(gpu)
        var metrics = Metrics()
        var failures: [String] = []
        var r4Load = PathTally()
        var native = 0, nativeRaw = 0, tieWrong = 0, edgeWrong = 0
        for turn in 1 ... 4 {
            for shape in Self.shapes {
                let cells = RotatedCells(
                    gpu: gpu, states: states, turn: turn, width: shape.width, height: shape.height,
                    aligned: shape.aligned
                )
                guard let plane = try cells.makePlane() else {
                    return .fail("the device makes no texture with the rotation SPI", metrics, proof: gpu.proof)
                }
                for cell in shape.cells {
                    let name = "r\(turn)_\(shape.name)_\(cell.rawValue)"
                    let result = try await cells.run(cell, into: plane)
                    if cell == .load && turn == 4 { r4Load.add(result.path) }
                    switch cell.kind {
                    case .native:
                        if raw {
                            native += 1
                            if result.path == "gm20b" {
                                nativeRaw += 1
                            } else {
                                failures.append("\(name) ran \(result.path)")
                            }
                        }
                        if result.wrong != 0 { failures.append("\(name): \(result.wrong) pixels off Metal's store") }
                    case .informational:
                        if cell == .edgeLoad { edgeWrong += result.wrong } else { tieWrong += result.wrong }
                    }
                }
            }
        }
        metrics.set("native_cells", native)
        metrics.set("native_raw", nativeRaw)
        metrics.set("tie_wrong", tieWrong)
        metrics.set("r4_edge_wrong", edgeWrong)
        metrics.set("r4_load_paths", r4Load.isEmpty ? "-" : r4Load.text)
        metrics.set("paths", gpu.paths.text)
        metrics.set("heap_full", (GM20BFacade.counters(of: gpu.device)["heap_full"] ?? 0) - heapFullBefore)
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}
