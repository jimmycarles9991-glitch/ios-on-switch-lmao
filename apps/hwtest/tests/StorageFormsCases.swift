import Foundation
import HWTestCore
import HWTestGPU
import Metal

enum StorageFormsCases {
    static let side = 16
    static let clear = MTLClearColor(red: 0.25, green: 0, blue: 0.5, alpha: 1)
    static let timedWidth = 1280, timedHeight = 720, timedTriangles = 64, timedRuns = 5
    static let steps: [Float] = (0 ..< 16).map { k in Float((k * 7) % 16 + 1) / 64 }

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct SFParams { float k[16]; float step; float z_step; };
    struct SFOut { float4 position [[position]]; uint prim [[flat]]; };

    vertex SFOut sf_triangles(uint vid [[vertex_id]], constant SFParams &p [[buffer(0)]]) {
        const float2 corners[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
        const uint t = vid / 3;
        return {float4(corners[vid % 3], 1.0 - float(t + 1) * p.z_step, 1.0), t};
    }

    static float4 sf_step(float4 dst, uint t, constant SFParams &p) {
        const float k = p.k[t % 16];
        return float4(dst.r * 0.5 + k, dst.g + p.step, fract(dst.b * 3.0 + k), 1.0 - dst.a);
    }

    fragment float4 sf_order(SFOut in [[stage_in]], float4 dst [[color(0)]], constant SFParams &p [[buffer(0)]]) {
        return sf_step(dst, in.prim, p);
    }

    struct SFDepthOut { float4 colour [[color(0)]]; float depth [[depth(any)]]; };
    fragment SFDepthOut sf_order_depth(SFOut in [[stage_in]], float4 dst [[color(0)]],
                                       constant SFParams &p [[buffer(0)]]) {
        return {sf_step(dst, in.prim, p), (in.prim & 1) ? 0.9 : 0.5 - float(in.prim % 16) / 64.0};
    }

    fragment float4 sf_order_discard(SFOut in [[stage_in]], float4 dst [[color(0)]],
                                     constant SFParams &p [[buffer(0)]]) {
        if ((in.prim % 3) == 1 && ((uint(in.position.x) ^ uint(in.position.y)) & 1)) discard_fragment();
        return sf_step(dst, in.prim, p);
    }

    struct SFScreen { float4 position [[position]]; };
    vertex SFScreen sf_screen(uint vid [[vertex_id]]) {
        const float2 corners[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
        return {float4(corners[vid], 0.5, 1)};
    }

    struct SFLoaded { float4 colour [[color(0)]]; float4 copy [[color(1)]]; };
    fragment SFLoaded sf_load_float(SFScreen in [[stage_in]], float4 dst [[color(0)]]) { return {dst, dst}; }

    struct SFLoadedHalf { half4 colour [[color(0)]]; float4 copy [[color(1)]]; };
    fragment SFLoadedHalf sf_load_half(SFScreen in [[stage_in]], half4 dst [[color(0)]]) {
        return {dst, float4(dst)};
    }

    fragment float4 sf_unpack(SFScreen in [[stage_in]], texture2d_ms<float, access::read> samples [[texture(0)]]) {
        const uint2 at = uint2(in.position.xy);
        return samples.read(uint2(at.x / 4, at.y), at.x % 4);
    }
    """

    enum Tests: String, CaseIterable { case depthwrite, pass, readonly, stencilwrite }

    struct Case {
        let key: String
        var fragment = "sf_order"
        let format: MTLPixelFormat
        var samples = 1
        var tests: Tests?
        let triangles: Int
        var even: [Int] = []
        var odd: [Int] = []
        var reported = false

        func expected(_ x: Int, _ y: Int) -> [Int] {
            let one = (x ^ y) & 1 == 1 && !odd.isEmpty ? odd : even
            return samples > 1 ? one + one : one
        }
    }

    static let cases: [Case] =
        [
            Case(key: "bgr5a1_order1", format: .bgr5A1Unorm, triangles: 1, even: [0x1030]),
            Case(key: "bgr5a1_order2", format: .bgr5A1Unorm, triangles: 2, even: [0x9855]),
            Case(key: "bgr5a1_overlap8", format: .bgr5A1Unorm, triangles: 8, even: [0x951d], reported: true),
            Case(key: "bgr5a1_overlap16", format: .bgr5A1Unorm, triangles: 16, even: [0xa204], reported: true),
        ]
        + Tests.allCases.flatMap { tests -> [Case] in
            let ordered = [202, 128, 66, 255]
            return [
                Case(
                    key: "depth_export_\(tests)", fragment: "sf_order_depth", format: .bgra8Unorm, tests: tests,
                    triangles: 16, even: tests == .depthwrite || tests == .stencilwrite ? [207, 64, 39, 255] : ordered
                ),
                Case(
                    key: "discard_\(tests)", fragment: "sf_order_discard", format: .bgra8Unorm, tests: tests,
                    triangles: 16, even: ordered, odd: [124, 88, 64, 0]
                ),
            ]
        }
        + [
            (1, [36, 8, 134, 0], [36, 8, 134, 0]), (2, [50, 17, 181, 255], [36, 8, 134, 0]),
            (8, [44, 66, 8, 255], [67, 41, 54, 0]), (16, [66, 131, 57, 255], [64, 91, 154, 0]),
        ].flatMap { n, drawn, discarded in
            [
                Case(key: "ms4_srgb_order\(n)", format: .bgra8Unorm_srgb, samples: 4, triangles: n, even: drawn),
                Case(
                    key: "ms4_srgb_discard\(n)", fragment: "sf_order_discard", format: .bgra8Unorm_srgb, samples: 4,
                    tests: .depthwrite, triangles: n, even: drawn, odd: discarded
                ),
            ]
        }

    static func timedCase(_ fragment: String, _ format: MTLPixelFormat, samples: Int = 1, tests: Tests?) -> Case {
        Case(
            key: "timed", fragment: fragment, format: format, samples: samples, tests: tests, triangles: timedTriangles
        )
    }

    static let timed: [(kind: String, storage: Case, twin: Case)] = [
        ("bgr5a1", timedCase("sf_order", .bgr5A1Unorm, tests: nil), timedCase("sf_order", .bgr5A1Unorm, tests: nil)),
        (
            "depth_export", timedCase("sf_order_depth", .bgra8Unorm, tests: .depthwrite),
            timedCase("sf_order_depth", .bgra8Unorm, tests: nil)
        ),
        (
            "discard", timedCase("sf_order_discard", .bgra8Unorm, tests: .depthwrite),
            timedCase("sf_order_discard", .bgra8Unorm, tests: nil)
        ),
        (
            "ms4_srgb", timedCase("sf_order_discard", .bgra8Unorm_srgb, samples: 4, tests: .depthwrite),
            timedCase("sf_order_discard", .bgra8Unorm_srgb, samples: 4, tests: nil)
        ),
    ]

    static func loadedTexel(_ i: Int) -> UInt16 {
        guard i < 64 else { return 0 }
        let c = i / 2
        return UInt16(c << 10 | (31 - c) << 5 | (7 * c) % 32 | (i % 2) << 15)
    }

    static func loadedLanes(_ texel: UInt16) -> [UInt32] {
        let codes = [Int(texel >> 10 & 31), Int(texel >> 5 & 31), Int(texel & 31)]
        return codes.map { Float(Float16(Float($0) / 31)).bitPattern } + [Float(texel >> 15).bitPattern]
    }

    struct Observation {
        var error: NSError?
        var path = "-"
        var pixels: [[Int]] = []
    }

    struct Loaded {
        var error: NSError?
        var path = "-"
        var lanesOff = 0
        var texelsChanged = 0
    }
}
