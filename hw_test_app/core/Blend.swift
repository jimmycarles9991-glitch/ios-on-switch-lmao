import Foundation

/// MTLBlendFactor's cases, raw values included.
public enum BlendFactor: Int, CaseIterable, Sendable {
    case zero, one, sourceColor, oneMinusSourceColor, sourceAlpha, oneMinusSourceAlpha, destinationColor
    case oneMinusDestinationColor, destinationAlpha, oneMinusDestinationAlpha, sourceAlphaSaturated, blendColor
    case oneMinusBlendColor, blendAlpha, oneMinusBlendAlpha, source1Color, oneMinusSource1Color, source1Alpha
    case oneMinusSource1Alpha

    public var dualSource: Bool { rawValue >= BlendFactor.source1Color.rawValue }
}

/// MTLBlendOperation's cases, raw values included.
public enum BlendOperation: Int, CaseIterable, Sendable {
    case add, subtract, reverseSubtract, min, max
}

public struct BlendState: Equatable, Sendable {
    public var rgbOperation: BlendOperation
    public var alphaOperation: BlendOperation
    public var sourceRGB: BlendFactor
    public var destinationRGB: BlendFactor
    public var sourceAlpha: BlendFactor
    public var destinationAlpha: BlendFactor

    public init(
        _ rgbOperation: BlendOperation = .add, source: BlendFactor, destination: BlendFactor,
        alphaOperation: BlendOperation? = nil, sourceAlpha: BlendFactor? = nil, destinationAlpha: BlendFactor? = nil
    ) {
        self.rgbOperation = rgbOperation
        self.alphaOperation = alphaOperation ?? rgbOperation
        sourceRGB = source
        destinationRGB = destination
        self.sourceAlpha = sourceAlpha ?? source
        self.destinationAlpha = destinationAlpha ?? destination
    }

    public var dualSource: Bool {
        [sourceRGB, destinationRGB, sourceAlpha, destinationAlpha].contains(where: \.dualSource)
    }

    public var name: String {
        let rgb = "\(rgbOperation)(\(sourceRGB),\(destinationRGB))"
        if alphaOperation == rgbOperation && sourceAlpha == sourceRGB && destinationAlpha == destinationRGB {
            return rgb
        }
        return rgb + "/\(alphaOperation)(\(sourceAlpha),\(destinationAlpha))"
    }
}

/// Metal's blend equation on the CPU, in RGBA. A normalized target clamps the fragment's outputs, the destination
/// and the result to [0, 1]; a float target (extended range) clamps nothing.
public enum Blend {
    public static func apply(
        _ state: BlendState, source: [Float], source1: [Float], destination: [Float], constant: [Float],
        normalized: Bool
    ) -> [Float] {
        let clamp = { (v: [Float]) in normalized ? v.map { min(max($0, 0), 1) } : v }
        let (s, s1, d) = (clamp(source), clamp(source1), clamp(destination))
        func factor(_ f: BlendFactor, _ c: Int) -> Float {
            let alpha = c == 3
            switch f {
            case .zero: return 0
            case .one: return 1
            case .sourceColor: return s[c]
            case .oneMinusSourceColor: return 1 - s[c]
            case .sourceAlpha: return s[3]
            case .oneMinusSourceAlpha: return 1 - s[3]
            case .destinationColor: return d[c]
            case .oneMinusDestinationColor: return 1 - d[c]
            case .destinationAlpha: return d[3]
            case .oneMinusDestinationAlpha: return 1 - d[3]
            case .sourceAlphaSaturated: return alpha ? 1 : min(s[3], 1 - d[3])
            case .blendColor: return constant[c]
            case .oneMinusBlendColor: return 1 - constant[c]
            case .blendAlpha: return constant[3]
            case .oneMinusBlendAlpha: return 1 - constant[3]
            case .source1Color: return s1[c]
            case .oneMinusSource1Color: return 1 - s1[c]
            case .source1Alpha: return s1[3]
            case .oneMinusSource1Alpha: return 1 - s1[3]
            }
        }
        let result = (0..<4).map { c -> Float in
            let alpha = c == 3
            let operation = alpha ? state.alphaOperation : state.rgbOperation
            let a = s[c] * factor(alpha ? state.sourceAlpha : state.sourceRGB, c)
            let b = d[c] * factor(alpha ? state.destinationAlpha : state.destinationRGB, c)
            switch operation {
            case .add: return a + b
            case .subtract: return a - b
            case .reverseSubtract: return b - a
            case .min: return min(s[c], d[c])
            case .max: return max(s[c], d[c])
            }
        }
        return clamp(result)
    }
}
