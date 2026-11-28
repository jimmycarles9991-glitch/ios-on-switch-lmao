import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// The facade's compression thread, waited for by its own event (drivers/TegraGM20BMetal/
/// TegraGM20BMetalProbe.mm switchMetalProbeCompressionSettled:, debug builds): the block runs once every compressed memory
/// and ZBC entry asked of the thread so far is made or refused, at once on the caller's thread when none is asked. Past
/// `bound` the event is taken as lost (a FAIL naming it), never waited for again.
public enum CompressionSettled {
    public static let selector = NSSelectorFromString("switchMetalProbeCompressionSettled:")
    public static let factsSelector = NSSelectorFromString("switchMetalProbeFacts:")
    public static let bound: TimeInterval = 5

    public enum Wait: Equatable, Sendable {
        /// A release facade or another device: no event to wait for
        case absent
        case settled(milliseconds: Double)
        /// The facade did not take the ask: its answer
        case refused(String)
        case never

        /// The test failure a wait that did not settle is, nil where it settled or there is no event
        public var failure: TestFailure? {
            switch self {
            case .absent, .settled: nil
            case let .refused(why): TestFailure("the facade refused the compression-settled event: \(why)")
            case .never: TestFailure("the compression thread never settled within \(Int(CompressionSettled.bound)) s")
            }
        }
    }

    /// Asks the facade for the event, `settled` the block it calls: nil where it took the ask, else its answer. The block
    /// goes as an object the facade may keep until the thread settles; lent as a closure to a cast of the method's IMP it
    /// would be non-escaping, and Swift traps once the call returns with the facade still holding it.
    public static func ask(_ facade: NSObject, _ settled: @escaping () -> Void) -> String? {
        let block: @convention(block) () -> Void = settled
        guard let answer = facade.perform(selector, with: unsafeBitCast(block, to: AnyObject.self)) else { return nil }
        let object = answer.takeUnretainedValue()
        return object as? String ?? String(describing: object)
    }

    public static func wait(_ device: MTLDevice) async -> Wait {
        guard let facade = device as? NSObject, facade.responds(to: selector) else { return .absent }
        let waiter = Waiter<Double>()
        let start = CFAbsoluteTimeGetCurrent()
        if let refusal = ask(facade, { waiter.resume((CFAbsoluteTimeGetCurrent() - start) * 1000) }) {
            return .refused(refusal)
        }
        guard let milliseconds = await waiter.wait(timeout: bound) else { return .never }
        return .settled(milliseconds: milliseconds)
    }

    /// `wait`, throwing where the event was refused or never came
    public static func settle(_ device: MTLDevice) async throws {
        if let failure = await wait(device).failure { throw failure }
    }

    /// What `make` makes, on compressed memory where the facade can bind some: a size's memory is made by the
    /// compression thread after the first texture that wants it, which binds none, so where the first one did not, a
    /// second is made once the thread settled. The first stays where `make` put it until the caller lets it go.
    public static func remade(_ device: MTLDevice, _ make: () throws -> MTLTexture) async throws -> MTLTexture {
        let first = try make()
        guard comptags(device, first) == 0 else { return first }
        try await settle(device)
        return try make()
    }

    /// comptags= of switchMetalProbeFacts: (nil without the selector)
    public static func comptags(_ device: MTLDevice, _ texture: MTLTexture) -> Int? {
        guard let facade = device as? NSObject, facade.responds(to: factsSelector) else { return nil }
        let line = facade.perform(factsSelector, with: texture)?.takeUnretainedValue() as? String ?? ""
        return line.contains("comptags=1") ? 1 : 0
    }
}
