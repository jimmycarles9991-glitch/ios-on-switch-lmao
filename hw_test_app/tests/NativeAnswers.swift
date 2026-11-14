import Foundation
import HWTestCore
import Metal

/// The answers the raw device gives above its identity, per build, as drivers/TegraGM20BMetal/raw/runtime/
/// raw_device_answers.h holds them (GM20B's counts from kits/SwitchGM20B caps): a test reads the device's answer, logs it
/// and fails where it is not this table's.
enum NativeAnswers {
    static var ios17: Bool { ProcessInfo.processInfo.operatingSystemVersion.majorVersion < 27 }

    static let viewports = 16
    static let tessellationFactor = 64
    /// The amplify macro's mapping words (SWITCH_GM20B_AMP_MAX): supportsVertexAmplificationCount: YES up to it
    static let amplifications = 8
    /// maxVertexAmplificationCount: 24B119 keeps its A13's limits table's 2
    static var amplificationCount: Int { ios17 ? amplifications : 2 }
    static let threadgroupMemory = 48 << 10
    /// Eight colour targets of RGBA32Float
    static let framebufferBits = 8 * 128
    /// GM20B's 32 generic vec4 inputs; 124 is Metal 27's own ceiling
    static var interpolants: Int { ios17 ? 128 : 124 }
    /// MTLArgumentBuffersTier2 (as Metal's tiers count, from 1), its IDs read through the handle tables
    static let argumentBuffersTier = 2
    /// maxIndirectTextures and maxIndirectSamplers: 24B119's A13's, which 21H461 answers too
    static let indirectResources = 500000
    /// 24B119's supportsLayeredRendering is _MTLDevice's base answer, which raw does not give (nil: logged, not checked)
    static var layeredRendering: Int? { ios17 ? 1 : nil }

    /// What a device query answers through its selector (nil where the device has no method for it)
    static func answer(_ device: MTLDevice, _ selector: String) -> Int? {
        let object = device as AnyObject as! NSObject
        guard object.responds(to: NSSelectorFromString(selector)) else { return nil }
        return (object.value(forKey: selector) as? NSNumber)?.intValue
    }

    /// The largest count supportsVertexAmplificationCount: answers YES for, up to 16
    static func largestAmplification(_ device: MTLDevice) -> Int {
        (1...16).last(where: device.supportsVertexAmplificationCount) ?? 0
    }

    /// Logs `selector`'s answer as `field` and gives a failure where it is not `want`
    static func check(
        _ device: MTLDevice, _ selector: String, _ want: Int, field: String, _ metrics: inout Metrics
    ) -> String? {
        let got = answer(device, selector)
        metrics.set(field, got ?? -1)
        return got == want ? nil : "\(selector) answers \(got.map(String.init) ?? "nothing"), the table \(want)"
    }
}
