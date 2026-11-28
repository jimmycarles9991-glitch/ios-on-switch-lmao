import Foundation
import HWTestCore
import XCTest

/// gpu.gm20b.standin_ms_resolve's two readbacks of each one-sample texture: a compute kernel and a fragment pass
final class SampleRouteReadingsTests: XCTestCase {
    func testRoutesThatDisagreeFailWhateverTheFormat() {
        var read = SampleRouteReadings()
        read.routesDiffer = 3
        let checks = read.checks("f85_x2")
        XCTAssertEqual(checks.map { $0.1 }, [true, true, false])
        var metrics = Metrics()
        read.record("f85_x2", &metrics)
        XCTAssertEqual(metrics.keys, [
            "f85_x2_samples_off_fragment", "f85_x2_samples_off", "f85_x2_routes_differ", "f85_x2_first_differ",
        ])
        XCTAssertEqual(metrics["f85_x2_routes_differ"], .integer(3))
    }

    func testARefusedComputeReadFailsAndIsCounted() {
        var read = SampleRouteReadings()
        read.computeRefusal = "compute.nokernel/4"
        let checks = read.checks("f44_x4")
        XCTAssertEqual(checks.map { $0.1 }, [true, false])
        XCTAssertTrue(checks[1].0.contains("compute.nokernel/4"))
        var metrics = Metrics()
        read.record("f44_x4", &metrics)
        XCTAssertEqual(metrics.keys, ["f44_x4_samples_off_fragment", "f44_x4_compute_refused"])
    }

    func testAgreeingRoutesThatKeepEveryCodePass() {
        XCTAssertTrue(SampleRouteReadings().checks("f11_x2").allSatisfy { $0.1 })
    }
}
