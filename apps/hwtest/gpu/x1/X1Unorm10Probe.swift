import Foundation
import HWTestCore
import Metal

/// gpu.x1.rop_unorm10_rounding (METALMAP d23): how a float colour becomes a 10-bit unorm code.
public enum X1Unorm10Probe {
    /// The ways a value could become a code of 1023, each from the value the fragment returns
    enum Rule: String, CaseIterable {
        case nearest, nearestF32 = "nearest_f32", chip, down
        case halfNearest = "half_nearest", halfChip = "half_chip"

        func code(_ v: Float) -> Int {
            switch self {
            case .nearest: return X1Reference.unorm(Double(v), top: 1023)
            case .nearestF32:
                return v.isNaN ? 0 : Int((min(max(v, 0), 1) * 1023).rounded(.toNearestOrEven))
            case .chip: return X1Reference.store(Double(v), bits: 10)
            case .down: return v.isNaN ? 0 : Int((min(max(Double(v), 0), 1) * 1023).rounded(.down))
            case .halfNearest: return X1Reference.unorm(Double(Float16(v)), top: 1023)
            case .halfChip: return X1Reference.store(Double(Float16(v)), bits: 10)
            }
        }
    }

    struct Arm {
        let name: String
        let blend: ((MTLRenderPipelineColorAttachmentDescriptor) -> Void)?
        /// Apple's rule for the arm, measured on an M4 Max
        let apple: Rule
    }

    static let arms = [
        Arm(name: "plain", blend: nil, apple: .nearest),
        Arm(
            name: "over",
            blend: {
                $0.sourceRGBBlendFactor = .sourceAlpha
                $0.destinationRGBBlendFactor = .oneMinusSourceAlpha
                $0.sourceAlphaBlendFactor = .one
                $0.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            }, apple: .nearest
        ),
        Arm(
            name: "max",
            blend: {
                $0.rgbBlendOperation = .max
                $0.alphaBlendOperation = .max
            }, apple: .nearest
        ),
    ]

    static let formats: [(name: String, format: MTLPixelFormat, redShift: UInt32)] = [
        ("rgb10a2", .rgb10a2Unorm, 0), ("bgr10a2", .bgr10a2Unorm, 20),
    ]

    static let width = 1024, offsets = 17, step = Float(1) / 131_072

    static let specials: [Float] = [
        0, 0.5, 1, -0.25, 1.25, .nan, .infinity, -.infinity, 1e-30, 1.0 / 2048, 1.0 / 4096, 0.25, 0.75, 1.0 / 1023,
        1022.0 / 1023, 1 - 1.0 / 16_777_216, 0.5 + 1.0 / 16_777_216,
    ]

    /// Columns 0 to 1022 hold code c's upper boundary (c + 0.5) / 1023 and the values 8 steps of 2^-17 each side of it,
    /// a row a step; column 1023 the specials.
    static let values: [Float] = (0 ..< (width * offsets)).map { i in
        let (x, y) = (i % width, i / width)
        if x == width - 1 { return specials[y] }
        return Float((Double(x) + 0.5) / 1023) + Float(y - offsets / 2) * step
    }

    /// The first rule every value fits, or "none" with the closest rule and its misses
    static func fitted(_ codes: [Int]) -> String {
        let misses = Rule.allCases.map { rule in zip(values, codes).filter { rule.code($0) != $1 }.count }
        if let exact = misses.firstIndex(of: 0) { return Rule.allCases[exact].rawValue }
        let closest = misses.indices.min { misses[$0] < misses[$1] }!
        return "none_\(Rule.allCases[closest].rawValue)_\(misses[closest])"
    }

    static func draw(_ gpu: GPU, _ format: MTLPixelFormat, _ arm: Arm) async throws -> (String, [UInt32]?) {
        let state = try gpu.render(fragment: "x1_value_fragment", format: format) { descriptor in
            guard let blend = arm.blend, let attachment = descriptor.colorAttachments[0] else { return }
            attachment.isBlendingEnabled = true
            blend(attachment)
        }
        let target = try gpu.texture(format, width, offsets)
        let data = try gpu.buffer(values.map { SIMD4<Float>($0, $0, $0, 1) })
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            var row = UInt32(width)
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBuffer(data, offset: 0, index: 0)
            encoder.setFragmentBytes(&row, length: 4, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        guard !timing.path.hasPrefix("error:") else { return (timing.path, nil) }
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        return (timing.path, (0 ..< (width * offsets)).map { bytes.loaded(as: UInt32.self, at: $0) })
    }

    /// Each format under each arm (Arm): unblended over a clear, which the translator lowers to Metal's code written as
    /// its nearest float for the ROP to store (the color write's StoreCode lane); source-over at alpha 1 and max over
    /// zero, which reach the ROP's blender as the fragment computed them (inferred from color_write's lowering). The values are column 1023's specials and,
    /// everywhere else, each code boundary (c + 0.5) / 1023 and 8 steps of 2^-17 (1/128 of a code) each side, from a
    /// buffer as floats. Per cell (<format>_<arm>): _rule, the first rule (Rule) every value fits; _wrong, the values
    /// whose code is not Apple's; _worst, the largest difference from Apple's code; _tie, the code of 0.5 (511.5, the
    /// one exact tie); _channels_apart, texels whose red, green and blue codes differ. rop10_rule is every cell's rule
    /// when they agree, rop10_worst_code the largest _worst. Apple's codes are the arm's rule an M4 Max fits with no
    /// miss: nearest (0.5 -> 512) in all three arms, specials included. chip, GM20B's store as the bench measured it,
    /// differs from nearest at 4,095 of the 17,391 boundary values. gpu.x1.rop_conversion sweeps the halves under max.
    public static func rounding(_ gpu: GPU) async throws -> ProbeFinding {
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var paths: [String] = []
        var rules: [String] = []
        var worst = 0
        let tieAt = (1 * width) + width - 1
        for format in formats {
            for arm in arms {
                let cell = "\(format.name)_\(arm.name)"
                let (path, words) = try await draw(gpu, format.format, arm)
                guard let words else {
                    metrics.set(cell + "_failed", path)
                    continue
                }
                paths.append(path)
                let channel = { (w: UInt32, shift: UInt32) in Int(w >> shift & 0x3ff) }
                let codes = words.map { channel($0, format.redShift) }
                let apart = words.filter { channel($0, 0) != channel($0, 10) || channel($0, 0) != channel($0, 20) }
                let off = zip(values, codes).map { abs(arm.apple.code($0) - $1) }
                let rule = fitted(codes)
                rules.append(rule)
                worst = max(worst, off.max() ?? 0)
                wrong[cell + "_wrong"] = off.filter { $0 != 0 }.count
                metrics.set(cell + "_rule", rule)
                metrics.set(cell + "_worst", off.max() ?? 0)
                metrics.set(cell + "_tie", codes[tieAt])
                metrics.set(cell + "_channels_apart", apart.count)
            }
        }
        metrics.set("rop10_rule", Set(rules).count == 1 ? rules[0] : rules.isEmpty ? "-" : "mixed")
        metrics.set("rop10_worst_code", worst)
        guard !paths.isEmpty else { return ProbeFinding(.inconclusive, metrics, note: "every cell's draw failed") }
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
    }
}
