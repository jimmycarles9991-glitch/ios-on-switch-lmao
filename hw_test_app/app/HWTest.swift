import Foundation
import HWTestCore

enum Subsystem: String, CaseIterable, Sendable {
    case metal = "Metal / GPU"
    case decode = "Video decode"
    case encode = "Video encode"
    case images = "Images"
    case audio = "Audio"
    case display = "Display"
    case compositor = "Compositor"
    case sensors = "Sensors"
    case input = "Input"
    case connectivity = "Connectivity"
    case storage = "Storage"
    case cpu = "CPU and crypto"
    case system = "System identity"
}

/// One test. `run` is called off the main thread; anything that touches UIKit hops to the main actor itself.
protocol HWTest: Sendable {
    var id: String { get }
    var name: String { get }
    var subsystem: Subsystem { get }
    /// Needs a person (a headset plug, a button press): the suite runs these after the rest, each behind a prompt
    /// that can be skipped.
    var interactive: Bool { get }
    /// The longest a run may take before the suite records a FAIL and moves on.
    var timeout: TimeInterval { get }
    func run(_ context: TestContext) async throws -> Outcome
}

extension HWTest {
    var interactive: Bool { false }
    var timeout: TimeInterval { 60 }
}
