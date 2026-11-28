import Foundation
import HWTestCore
import Metal

/// The facade's debug probes and counters as gpu.perf.knobs and gpu.perf.nvn read them (TegraGM20BMetalProbe.mm). Off
/// the facade, or in a release runtime, every probe answers nil and every counter is absent.
extension GPURates {
    static let probesOff: [(String, UInt32)] = [
        ("switchMetalProbeNvn:", 0), ("switchMetalProbeZcullCriterion:", 0), ("switchMetalProbeMs4Cbr:", 0),
        ("switchMetalProbeSectorPromotion:", 0), ("switchMetalProbePlainPrivate:", 0), ("switchMetalProbeDirectPa:", 0),
        ("switchMetalProbeZcullKeep:", 0),
    ]

    /// The probe's answer after setting it to `value`; nil where the device has no such probe
    @discardableResult
    func probe(_ name: String, _ value: UInt32) -> String? {
        let selector = NSSelectorFromString(name)
        guard let object = gpu.device as? NSObject, object.responds(to: selector) else { return nil }
        return object.perform(selector, with: NSNumber(value: value))?.takeUnretainedValue() as? String ?? ""
    }

    @discardableResult
    func ask(_ object: AnyObject, _ name: String, _ argument: Any? = nil) -> String? {
        let selector = NSSelectorFromString(name)
        guard let object = object as? NSObject, object.responds(to: selector) else { return nil }
        let answer = argument.map { object.perform(selector, with: $0) } ?? object.perform(selector)
        return answer?.takeUnretainedValue() as? String
    }

    func has(_ name: String) -> Bool { (gpu.device as? NSObject)?.responds(to: NSSelectorFromString(name)) ?? false }

    /// A probe line's `key=value` fields, decimal or 0x-prefixed hex
    static func fields(_ line: String?) -> [String: Int] {
        var out: [String: Int] = [:]
        for field in (line ?? "").split(separator: " ") {
            let pair = field.split(separator: "=", maxSplits: 1)
            guard pair.count == 2 else { continue }
            let value = pair[1].hasPrefix("0x") ? Int(pair[1].dropFirst(2), radix: 16) : Int(pair[1])
            if let value { out[String(pair[0])] = value }
        }
        return out
    }

    func facts(_ texture: MTLTexture) -> [String: Int] {
        Self.fields(ask(gpu.device, "switchMetalProbeFacts:", texture))
    }

    /// Where a completed command buffer's barriers took its forms (probe_forms.h ProbeTaken)
    func taken(_ c: MTLCommandBuffer) -> [String: Int] { Self.fields(ask(c, "switchMetalProbeTaken")) }

    /// Word `index` of the header the texture unit reads for `texture`; nil where the driver holds none
    func headerWord(_ texture: MTLTexture, _ index: Int) -> UInt32? {
        guard let words = ask(gpu.device, "switchMetalProbeHeader:", texture)?.split(separator: "."),
              words.count > index
        else { return nil }
        return UInt32(words[index], radix: 16)
    }

    func forms(_ c: MTLCommandBuffer, _ forms: String) throws {
        guard (c as? NSObject)?.responds(to: NSSelectorFromString("switchMetalProbeForms:")) == true else {
            throw TestFailure("the facade has no switchMetalProbeForms:")
        }
        if let refusal = ask(c, "switchMetalProbeForms:", forms) { throw TestFailure(refusal) }
    }

    /// How far each of `keys` among the facade's counters grew over `body`
    func grown(_ keys: [String], _ body: () async throws -> Void) async throws -> [String: Int] {
        let before = GM20BFacade.counters(of: gpu.device)
        try await body()
        let after = GM20BFacade.counters(of: gpu.device)
        return Dictionary(uniqueKeysWithValues: keys.map { ($0, (after[$0] ?? 0) - (before[$0] ?? 0)) })
    }

    /// `body`, a failure in it flagged as `<arm>_error` and its text a problem (the test's why=), then every probe off,
    /// so the arms after it run
    func isolated(_ arm: String, _ body: () async throws -> Void) async {
        do {
            try await body()
        } catch {
            metrics.set("\(arm)_error", true)
            problems.append("\(arm): \(error)")
        }
        for (name, value) in Self.probesOff { probe(name, value) }
    }
}
