import Foundation
import Metal

/// A chain format of the mip HW Tests and how its texels' bits hold their channels
public struct MipFormat {
    public let name: String
    public let pixel: MTLPixelFormat
    public let layout: MipLayout

    public init(name: String, pixel: MTLPixelFormat, layout: MipLayout) {
        self.name = name
        self.pixel = pixel
        self.layout = layout
    }
}

public enum MipFormats {
    /// The formats fx::twodFormat gives the 2D engine (gpu.raw.mip_twod)
    public static let twod = [
        MipFormat(name: "bgra8", pixel: .bgra8Unorm, layout: .unorm([8, 8, 8, 8])),
        MipFormat(name: "rgba8", pixel: .rgba8Unorm, layout: .unorm([8, 8, 8, 8])),
        MipFormat(name: "rgba16f", pixel: .rgba16Float, layout: .floats([16, 16, 16, 16])),
        MipFormat(name: "r8", pixel: .r8Unorm, layout: .unorm([8])),
        MipFormat(name: "rg8", pixel: .rg8Unorm, layout: .unorm([8, 8])),
        MipFormat(name: "bgra8srgb", pixel: .bgra8Unorm_srgb, layout: .unorm([8, 8, 8, 8], srgb: true)),
        MipFormat(name: "rgba8srgb", pixel: .rgba8Unorm_srgb, layout: .unorm([8, 8, 8, 8], srgb: true)),
    ]

    /// The pack's mip state formats (gpu.raw.mip_twod_candidates)
    public static let candidates = [
        MipFormat(name: "a8", pixel: .a8Unorm, layout: .unorm([8])),
        MipFormat(name: "r16", pixel: .r16Unorm, layout: .unorm([16])),
        MipFormat(name: "r16f", pixel: .r16Float, layout: .floats([16])),
        MipFormat(name: "b5g6r5", pixel: .b5g6r5Unorm, layout: .unorm([5, 6, 5])),
        MipFormat(name: "bgr5a1", pixel: .bgr5A1Unorm, layout: .unorm([5, 5, 5, 1])),
        MipFormat(name: "r32f", pixel: .r32Float, layout: .floats([32])),
        MipFormat(name: "rg16", pixel: .rg16Unorm, layout: .unorm([16, 16])),
        MipFormat(name: "rg16f", pixel: .rg16Float, layout: .floats([16, 16])),
        MipFormat(name: "rgb10a2", pixel: .rgb10a2Unorm, layout: .unorm([10, 10, 10, 2])),
        MipFormat(name: "rg11b10f", pixel: .rg11b10Float, layout: .floats([11, 11, 10])),
        MipFormat(name: "bgr10a2", pixel: .bgr10a2Unorm, layout: .unorm([10, 10, 10, 2])),
        MipFormat(name: "rg32f", pixel: .rg32Float, layout: .floats([32, 32])),
        MipFormat(name: "rgba16", pixel: .rgba16Unorm, layout: .unorm([16, 16, 16, 16])),
        MipFormat(name: "rgba32f", pixel: .rgba32Float, layout: .floats([32, 32, 32, 32])),
    ]
}
