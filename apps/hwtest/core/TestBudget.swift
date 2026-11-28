import Foundation

/// How long one unattended test may take (the owner, 25 Nov: Run All's tests run in seconds; past 15 s is a design or a
/// driver to fix). A test past it carries `over_budget=1` in its line, and suite.harness names it; it is not failed for
/// it. A test that waits for a person is not held to it.
public enum TestBudget {
    public static let key = "over_budget"
    public static let milliseconds = 15_000.0

    public static func over(_ milliseconds: Double, interactive: Bool) -> Bool {
        !interactive && milliseconds > Self.milliseconds
    }
}
