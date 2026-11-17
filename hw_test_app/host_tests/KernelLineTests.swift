import Foundation
import HWTestCore
import XCTest

final class KernelLineTests: XCTestCase {
    /// gpu.rail_cycle's sysctl line on boot 22 (13:46:59), whose why= made the test a harness FAIL.
    static let railCycle =
        "cycles=4 downs=4 fences=4 plls=4 wakes_off_rate=0 wake_coeff=00012801 fence_us_max=62 coeff=00012801 "
        + "cfg=00020001 pl=1 expect_pl=1 khz=768000 why=-"

    func testAKeyTheLogLineKeepsForItselfIsLoggedAsTheKernels() {
        let fields = KernelLine.fields(Self.railCycle)
        let metrics = KernelLine.metrics(fields, keys: ["downs", "fences", "why"])
        XCTAssertEqual(metrics.faults, [])
        XCTAssertEqual(metrics["kernel_why"], .text("-"))
        XCTAssertEqual(metrics["downs"], .text("4"))
        let outcome = Outcome.judged([("downs = cycles", fields["downs"] == "4")], metrics).withHarnessFaults
        XCTAssertEqual(outcome.verdict, .pass, outcome.detail)
        XCTAssertTrue(
            LogLine.format(id: "gpu.rail_cycle", outcome: outcome, milliseconds: 143).contains(" kernel_why=- "))
    }

    func testFieldsAreKeyValueTokensOnly() {
        XCTAssertEqual(KernelLine.fields("a=1 b= c=x=y loose"), ["a": "1", "b": "", "c": "x=y"])
    }
}
