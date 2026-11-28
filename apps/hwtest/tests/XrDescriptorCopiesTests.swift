import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// Copies between RGB10A8_2P_XR10 textures a descriptor made (17x4, 16x8, 2x8, 33x5; Shared and Private) and one over a
/// 'b3a8' 32x8 (gpu/XrDescriptorCopies.swift): surface to descriptor, descriptor to surface and between descriptors,
/// sub-rectangles, widths under 4 both ways. Each completes and leaves its destination as an M4 Max does under macOS
/// 27.2.1 (this scene on it, three runs, every case 0 off; the rule its 287-case descriptor-copy probe fits): a surface
/// byte for byte, a descriptor's texels by a kernel's read (codes and alpha), the source unchanged.
/// xr_desc_copy_off_<case>_<storage> (bytes or channels off), xr_desc_copy_status (completed of 22).
struct XrDescriptorCopiesTest: HWTest {
    let id = "gpu.gm20b.xr_descriptor_copies"
    let name = "Copies between descriptor-made and surface RGB10A8_2P_XR10 textures move Metal's bytes, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            var completed = 0
            for (storage, name) in [(MTLStorageMode.shared, "shared"), (.private, "private")] {
                for c in XrDescriptorCopies.cases {
                    let key = "xr_desc_copy_off_\(c.key)_\(name)"
                    do {
                        let r = try await XrDescriptorCopies.run(gpu, c, storage: storage)
                        metrics.set(key, r.off)
                        if r.path == "gm20b" { completed += 1 }
                        checks += [
                            ("\(c.key) \(name): \(r.path)", r.path == "gm20b"),
                            ("\(c.key) \(name): \(r.off) off of \(r.moved)", r.off == 0),
                            ("\(c.key) \(name): source \(r.sourceOff) off", r.sourceOff == 0),
                        ]
                    } catch {
                        metrics.set(key, -1)
                        checks.append(("\(c.key) \(name): \(GPU.describe(error))", false))
                    }
                }
            }
            metrics.set("xr_desc_copy_status", completed)
            return checks
        }
    }
}

/// A surface's region copied into a descriptor-made RGB10A8_2P_XR10 (17x4 and 33x5, Shared and Private) and back out
/// into a second surface at the same coordinates (gpu/XrDescriptorCopies.swift roundTrip): an M4 Max keeps the bytes
/// its first copy writes into the descriptor's row padding and past its rows, so the second surface's region holds
/// the first's (this scene on it under macOS 27.2.1, three runs, 0 off of 320 and 800 bytes). The driver holds a
/// descriptor 550 as 8-byte texels with no padding: those bytes come back 0. xr_desc_padding_off_<width>_<storage>.
struct XrDescriptorPaddingTest: HWTest {
    let id = "gpu.gm20b.xr_descriptor_padding"
    let name = "Bytes a copy leaves in a descriptor-made RGB10A8_2P_XR10's row padding come back out, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for (storage, name) in [(MTLStorageMode.shared, "shared"), (.private, "private")] {
                for (w, h, size) in [(17, 4, (16, 4)), (33, 5, (32, 5))] {
                    let key = "xr_desc_padding_off_\(w)_\(name)"
                    do {
                        let r = try await XrDescriptorCopies.roundTrip(
                            gpu, width: w, height: h, size: size, storage: storage
                        )
                        metrics.set(key, r.off)
                        checks += [
                            ("\(w) \(name): \(r.path)", r.path == "gm20b"), ("\(w) \(name): \(r.off) off", r.off == 0),
                        ]
                    } catch {
                        metrics.set(key, -1)
                        checks.append(("\(w) \(name): \(GPU.describe(error))", false))
                    }
                }
            }
            return checks
        }
    }
}
