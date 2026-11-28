import Foundation
import HWTestGPU
import Metal

/// The facade's reflections (drivers/TegraGM20BMetal/TegraGM20BMetalReflection.mm): asked with argument info,
/// a state's bindings come from the pack's blob or the system shader compiler's.
public enum ReflectionCheck {
    public static func describe(_ bindings: [MTLBinding]) -> String {
        bindings.isEmpty ? "-" : bindings.map { "\($0.name):\($0.index):\($0.type.rawValue)" }.joined(separator: ",")
    }

    // Synchronous on purpose: inside an async run Swift resolves the tuple-returning calls to Metal's completion-handler
    // forms
    public static func render(
        _ device: MTLDevice, _ descriptor: MTLRenderPipelineDescriptor
    ) throws
        -> MTLRenderPipelineReflection?
    {
        try device.makeRenderPipelineState(descriptor: descriptor, options: [.bindingInfo, .bufferTypeInfo]).1
    }

    public static func compute(_ device: MTLDevice, _ function: MTLFunction) throws -> MTLComputePipelineReflection? {
        var reflection: MTLComputePipelineReflection?
        _ = try device.makeComputePipelineState(
            function: function, options: [.bindingInfo, .bufferTypeInfo], reflection: &reflection
        )
        return reflection
    }
}

/// Each pipeline binding's answers, selector by selector, as gpu.gm20b.reflection_kinds compares them with Metal's
/// (ReflectionKindsGoldens): the selectors every binding answers, those of its own kind, and the pointer and struct
/// types every binding has a method for (reported). Read by sending each selector, so a binding of another kind than
/// Swift's protocol cast would allow is asked as an app asks it.
public enum ReflectionKinds {
    public static let common = ["type", "access", "index", "arrayLength", "isUsed", "isActive", "isArgument"]
    /// By MTLBindingType: buffer, threadgroup memory, texture, sampler
    public static let own: [Int: [String]] = [
        0: ["bufferAlignment", "bufferDataSize", "bufferDataType", "bufferStructType"],
        1: ["threadgroupMemoryAlignment", "threadgroupMemoryDataSize"],
        2: ["textureType", "textureDataType", "isDepthTexture"],
        3: [],
    ]
    public static let reported = ["bufferPointerType", "structType"]
    /// An object payload binding's own (MTLBindingType 34): no state here has one, so every binding asks them as
    /// another type's
    public static let payload = ["objectPayloadAlignment", "objectPayloadDataSize"]
    /// MTLBindingInternal's 21: Metal's binding of every type has a method for each, and reports a failure on another
    /// type's
    public static let responding = ["name"] + common + own.keys.sorted().flatMap { own[$0]! } + payload + reported
    public static let notResponding = ["dataType", "offset", "elementType", "members"]
    static let objects: Set<String> = ["bufferStructType", "bufferPointerType", "structType"]
    static let flags: Set<String> = ["isUsed", "isActive", "isArgument", "isDepthTexture"]

    /// One binding of a made pipeline: "<function>.<name>"
    public struct Seen {
        public let key: String
        public let name: String
        public let type: Int
        public let object: NSObject
    }

    public static func seen(_ function: String, _ bindings: [MTLBinding]) -> [Seen] {
        bindings.map { binding in
            let object = binding as AnyObject as! NSObject
            return Seen(
                key: "\(function).\(binding.name)", name: binding.name, type: answer(object, "type"), object: object
            )
        }
    }

    /// `selector`'s answer: a number, a flag as 1 or 0, an object's presence as 1 or 0
    public static func answer(_ object: NSObject, _ selector: String) -> Int {
        let sel = NSSelectorFromString(selector)
        if objects.contains(selector) { return object.perform(sel)?.takeUnretainedValue() == nil ? 0 : 1 }
        if flags.contains(selector) {
            typealias Flag = @convention(c) (NSObject, Selector) -> Bool
            return unsafeBitCast(object.method(for: sel), to: Flag.self)(object, sel) ? 1 : 0
        }
        typealias Number = @convention(c) (NSObject, Selector) -> UInt
        return Int(unsafeBitCast(object.method(for: sel), to: Number.self)(object, sel))
    }

    /// The selectors a binding of its kind answers, and the reported ones
    public static func answers(_ seen: Seen) -> [String: Int] {
        let asked = common + (own[seen.type] ?? []) + reported
        return Dictionary(uniqueKeysWithValues: asked.map { ($0, answer(seen.object, $0)) })
    }

    public struct Made {
        public var seen: [Seen] = []
        public var functions: Set<String> = []
        /// "<function>:<why>" for each state not made or made with no reflection
        public var unmade: [String] = []
    }

    /// reflection_kinds's compute state, reflection_vs and reflection_fs's render state (colour 0 BGRA8Unorm), and a
    /// compute state of each of `system`'s kernels of the library `systemLibrary` gives, made with binding and buffer
    /// type info
    public static func made(
        _ device: MTLDevice, _ library: MTLLibrary, system: [String], _ systemLibrary: () throws -> MTLLibrary
    ) -> Made {
        var out = Made()
        func compute(_ name: String, _ from: MTLLibrary) {
            do {
                guard let function = from.makeFunction(name: name) else { return out.unmade.append("\(name):absent") }
                guard let reflection = try ReflectionCheck.compute(device, function) else {
                    return out.unmade.append("\(name):no_reflection")
                }
                out.seen += seen(name, reflection.bindings)
                out.functions.insert(name)
            } catch {
                out.unmade.append("\(name):\(GPU.describe(error).prefix(60))")
            }
        }
        compute("reflection_kinds", library)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "reflection_vs")
        descriptor.fragmentFunction = library.makeFunction(name: "reflection_fs")
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        do {
            if let reflection = try ReflectionCheck.render(device, descriptor) {
                out.seen += seen("reflection_vs", reflection.vertexBindings)
                out.seen += seen("reflection_fs", reflection.fragmentBindings)
                out.functions.formUnion(["reflection_vs", "reflection_fs"])
            } else {
                out.unmade.append("reflection_vs:no_reflection")
            }
        } catch {
            out.unmade.append("reflection_vs:\(GPU.describe(error).prefix(60))")
        }
        guard !system.isEmpty else { return out }
        do {
            let from = try systemLibrary()
            for name in system { compute(name, from) }
        } catch {
            out.unmade.append("system_library:\(GPU.describe(error).prefix(60))")
        }
        return out
    }

    public struct Compared: Sendable {
        public var bindings = 0, mismatches = 0, arrayLengthMismatches = 0, respondsMismatches = 0, reportedOff = 0
        public var firstMismatch = "-"
    }

    /// `seen` against `expected` (every key of it whose function is in `functions`): a binding missing or extra, or a
    /// judged selector's answer off, is a mismatch; the reported selectors count apart
    public static func compare(
        _ seen: [Seen], _ expected: [String: [String: Int]], functions: Set<String>
    ) -> Compared {
        var out = Compared()
        let of = { (key: String) in functions.contains(String(key.prefix { $0 != "." })) }
        let wanted = expected.filter { of($0.key) }
        let byKey = Dictionary(seen.filter { of($0.key) }.map { ($0.key, $0) }, uniquingKeysWith: { first, _ in first })
        func off(_ what: String) {
            out.mismatches += 1
            if out.firstMismatch == "-" { out.firstMismatch = what }
        }
        for key in Set(byKey.keys).subtracting(wanted.keys).sorted() { off("\(key):extra") }
        for (key, want) in wanted.sorted(by: { $0.key < $1.key }) {
            guard let binding = byKey[key] else {
                off("\(key):missing")
                continue
            }
            out.bindings += 1
            let got = answers(binding)
            for (selector, value) in want.sorted(by: { $0.key < $1.key }) where got[selector] != value {
                if reported.contains(selector) {
                    out.reportedOff += 1
                    continue
                }
                if selector == "arrayLength" { out.arrayLengthMismatches += 1 }
                off("\(key).\(selector):\(got[selector].map(String.init) ?? "-")/\(value)")
            }
            for selector in responding where !binding.object.responds(to: NSSelectorFromString(selector)) {
                out.respondsMismatches += 1
            }
            for selector in notResponding where binding.object.responds(to: NSSelectorFromString(selector)) {
                out.respondsMismatches += 1
            }
        }
        return out
    }
}
