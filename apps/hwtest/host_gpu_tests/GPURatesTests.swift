import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// gpu.perf.rates' arms at small sizes on this Mac's GPU: each runs, is timed and gives a finite rate above 0, so a
/// broken arm fails here and not on the Switch.
final class GPURatesTests: XCTestCase {
    func testEveryArmGivesARate() async throws {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "GPURates.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let library = try await device.makeLibrary(source: sources.joined(separator: "\n"), options: nil)
        let result = try await GPURates.run(GPU(device: device, queue: queue, library: library), .host)
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        let rates = [
            "alu_gflops", "lane_mhz", "copy_gbps", "read_gbps_private", "read_gbps_private_pages", "read_gbps_shared",
            "read_gbps_shared_pages", "fill_gpix_s", "blend_gpix_s", "fill_shared_gpix_s", "fill4x_gpix_s",
            "depth_gpix_s", "texel_gtexel_s", "tri_mtri_s", "clipped_mtri_s", "hidden_us_per_layer",
            "shaded_us_per_layer", "atomic_ns_cached", "fetch_mtri_s_cached", "table_us_cached",
            "jobs_us_cached",
        ]
        let line = result.metrics.ordered.map { "\($0)=\($1.logText)" }.joined(separator: " ")
        print("GPURates \(HostFacts.fields(device)) \(line)")
        for key in rates {
            guard case .number(let value, _) = result.metrics[key] else {
                XCTFail("\(key) missing")
                continue
            }
            XCTAssertTrue(value.isFinite && value > 0, "\(key)=\(value)")
        }
    }

    /// gpu.perf.knobs at small sizes: with no probes on this Mac, the default knob and no promotion, each arm timed
    func testTheKnobArmsRunWithoutTheProbes() async throws {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "GPURates.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let library = try await device.makeLibrary(source: sources.joined(separator: "\n"), options: nil)
        let result = try await GPURates.knobs(GPU(device: device, queue: queue, library: library), .host)
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        XCTAssertEqual(result.notes.count, 2, "both probes are absent here")
        for key in [
            "subtile_one_default_us", "subtile_four_default_us", "subtile_depth_default_us", "subtile_fill_default_us",
            "promo_minified_0_us", "promo_one_0_us", "promo_scatter_0_us",
        ] {
            guard case .number(let value, _) = result.metrics[key] else {
                XCTFail("\(key) missing")
                continue
            }
            XCTAssertTrue(value.isFinite && value > 0, "\(key)=\(value)")
        }
    }

    /// gpu.perf.first_touch on this Mac: no probes, so each arm's on form is its off form, and the pixel checks find
    /// nothing wrong
    func testTheFirstTouchArmsRunWithoutTheProbes() async throws {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "GPURates.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let library = try await device.makeLibrary(source: sources.joined(separator: "\n"), options: nil)
        let result = try await GPURates.firstTouch(GPU(device: device, queue: queue, library: library), .host)
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        XCTAssertEqual(result.notes.count, 2, "both probes are absent here")
        guard case .integer(let wrong) = result.metrics["zcull_4x2_off_wrong"] else {
            return XCTFail("zcull_4x2_off_wrong missing")
        }
        XCTAssertEqual(wrong, 0)
        XCTAssertNotNil(result.metrics["discard_off_kept_px"])
    }

    /// gpu.perf.fill on this Mac: no probe and no band budget, so the SMMU cells at budget 0 alone, each timed
    func testTheFillCellsRunWithoutTheProbe() async throws {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "GPURates.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let library = try await device.makeLibrary(source: sources.joined(separator: "\n"), options: nil)
        let result = try await GPURates.fill(
            GPU(device: device, queue: queue, library: library), .host, productionBudget: nil
        )
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        XCTAssertEqual(result.notes.count, 1, "the probe is absent here")
        for key in [
            "fill_fresh_smmu_b0_gpix_s", "fill_plain_smmu_b0_gpix_s", "fill_comp_b0_gpix_s", "lane_mhz_first",
            "lane_mhz_last",
        ] {
            guard case .number(let value, _) = result.metrics[key] else {
                XCTFail("\(key) missing")
                continue
            }
            XCTAssertTrue(value.isFinite && value > 0, "\(key)=\(value)")
        }
    }

    /// gpu.perf.nvn on this Mac: no probes, so each arm's on form is its off form; the pixel checks must find nothing
    /// wrong, which proves their expected pictures
    func testTheNvnArmsNoteTheMissingProbe() async throws {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "GPURates.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let library = try await device.makeLibrary(source: sources.joined(separator: "\n"), options: nil)
        let result = try await GPURates.nvn(GPU(device: device, queue: queue, library: library), .host)
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        XCTAssertEqual(result.notes.count, 1)
        for key in [
            "zcull_bounds_hidden_off_wrong", "zcull_bounds_hidden_on_wrong", "stencil_zcull_off_wrong",
            "zcull_fp_off_wrong", "zcull_bounds_seen_culled_px", "cbr_bgra8_differ_bytes", "cbr_rgba16f_differ_bytes",
            "tiled_flush_off_wrong", "tiled_flush_on_wrong", "zcull_always_off_culled_px",
            "zcull_not_equal_off_culled_px", "zcull_rule_control_off_culled_px",
        ] {
            guard case .integer(let value) = result.metrics[key] else {
                XCTFail("\(key) missing")
                continue
            }
            XCTAssertEqual(value, 0, key)
        }
    }
}
