import HWTestCore
import Metal

/// One draw of a fetch shape: its vertices are points[vertex_id] (shaders/FramebufferFetch.metal fetch_count_vertex), in
/// clip space, so vertexStart and the indices choose them.
struct FetchDraw {
    var primitive: MTLPrimitiveType
    var points: [SIMD4<Float>]
    var indices: [UInt16]? = nil
    var vertexStart = 0
    var count: Int
    var instances = 1
    var indirect = false

    /// The primitives the facade counts for the draw (fetch_order.h primitives: a strip's restarts left in), or 0 for
    /// an indirect one, whose count only the GPU reads
    var counted: Int {
        if indirect { return 0 }
        let made: Int
        switch primitive {
        case .point: made = count
        case .line: made = count / 2
        case .lineStrip: made = max(count - 1, 0)
        case .triangle: made = count / 3
        default: made = max(count - 2, 0)
        }
        return made * instances
    }
}

/// A 16-bit target of the fetch scenes: blue in bits 0-4, green in the `greenBits` above, red in the five above those
/// (fill_texel.h), and how its fetching states keep raster order. NVK grants neither format storage access;
/// BGR5A1Unorm's storage form views it as R16_UINT (the translator's fetch/packed.rs), B5G6R5Unorm has none.
struct FetchTarget {
    let name: String
    let format: MTLPixelFormat
    let greenBits: Int
    let route: FetchRoute

    static let all = [
        FetchTarget(name: "bgr5a1", format: .bgr5A1Unorm, greenBits: 5, route: .interlock),
        FetchTarget(name: "b5g6r5", format: .b5g6r5Unorm, greenBits: 6, route: .macro),
    ]

    func codes(_ bytes: [UInt8]) -> [SIMD3<UInt8>] {
        let green = UInt16(1 << greenBits - 1)
        return stride(from: 0, to: bytes.count - 1, by: 2).map { at in
            let word = UInt16(bytes[at]) | UInt16(bytes[at + 1]) << 8
            return SIMD3(UInt8((word >> (5 + greenBits)) & 31), UInt8((word >> 5) & green), UInt8(word & 31))
        }
    }

    /// A scene's codes on this target: its green, written as code / 31, in the target's green bits
    func expected(_ codes: [SIMD3<UInt8>]) -> [SIMD3<UInt8>] {
        let top = Double(1 << greenBits - 1)
        return codes.map { SIMD3($0.x, UInt8((Double($0.y) * top / 31).rounded()), $0.z) }
    }
}

/// A destination-reading scene on an 8x8 (or larger) target cleared to 0: its draws and the 5-bit codes (red, green,
/// blue) each pixel must end with, row by row from the top. An `icb` scene's draws are the commands of one indirect
/// command buffer that inherits the pass's state and vertex buffer, so they share their draws' first points.
struct FetchShape {
    let side: Int
    var vertex = "fetch_count_vertex"
    var fragment = "fetch_count_fragment"
    var cull: MTLCullMode = .none
    var icb = false
    let draws: [FetchDraw]
    let codes: [SIMD3<UInt8>]

    static let restart: UInt16 = 0xffff

    /// A point in pixels (y down) as clip space
    static func clip(_ x: Float, _ y: Float, side: Int) -> SIMD4<Float> {
        SIMD4(x / Float(side) * 2 - 1, 1 - y / Float(side) * 2, 0.5, 1)
    }

    static let screen: [SIMD4<Float>] = [[-1, -1, 0.5, 1], [3, -1, 0.5, 1], [-1, 3, 0.5, 1]]

    static func uniform(_ side: Int, _ code: SIMD3<UInt8>) -> [SIMD3<UInt8>] {
        [SIMD3<UInt8>](repeating: code, count: side * side)
    }

    /// The triangles a list or strip makes, in the order a strip winds them: an odd triangle of a run takes its first
    /// two vertices swapped, and a restart index begins a run
    static func triangles(_ primitive: MTLPrimitiveType, _ indices: [Int]) -> [[Int]] {
        if primitive == .triangle {
            return stride(from: 0, to: indices.count - 2, by: 3).map { Array(indices[$0 ..< $0 + 3]) }
        }
        var made: [[Int]] = []
        var run: [Int] = []
        for index in indices + [Int(restart)] {
            if index == Int(restart) {
                for k in 0 ..< max(run.count - 2, 0) {
                    made.append(k % 2 == 0 ? [run[k], run[k + 1], run[k + 2]] : [run[k + 1], run[k], run[k + 2]])
                }
                run = []
            } else {
                run.append(index)
            }
        }
        return made
    }

    /// Twice a triangle's signed area in window pixels (y down): positive is clockwise on screen, Metal's default front
    static func windowArea(_ t: [SIMD2<Float>]) -> Float {
        (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[1].y - t[0].y) * (t[2].x - t[0].x)
    }

    /// Per pixel, the triangles of `draw` that cover its centre and are not culled
    static func coverage(_ draw: FetchDraw, side: Int, cull: MTLCullMode) -> [Int] {
        let indices = draw.indices.map { $0.map(Int.init) } ?? Array(draw.vertexStart ..< draw.vertexStart + draw.count)
        let window = draw.points.map { p in
            SIMD2((p.x / p.w + 1) / 2 * Float(side), (1 - p.y / p.w) / 2 * Float(side))
        }
        var counts = [Int](repeating: 0, count: side * side)
        for triangle in triangles(draw.primitive, indices) {
            let t = triangle.map { window[$0] }
            let area = windowArea(t)
            if area == 0 || (cull == .back && area < 0) || (cull == .front && area > 0) { continue }
            for y in 0 ..< side {
                for x in 0 ..< side {
                    let c = SIMD2(Float(x) + 0.5, Float(y) + 0.5)
                    let edges = (0 ..< 3).map { i -> Float in
                        let a = t[i], b = t[(i + 1) % 3]
                        return ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * (area > 0 ? 1 : -1)
                    }
                    if edges.allSatisfy({ $0 > 0 }) { counts[y * side + x] += draw.instances }
                }
            }
        }
        return counts
    }

    static func reds(_ counts: [Int]) -> [SIMD3<UInt8>] { counts.map { SIMD3(UInt8($0), 0, 0) } }
}

extension FetchShape {
    /// fetch_list: 8 full-screen triangles; every pixel at red 8
    static let list = FetchShape(
        side: 8, draws: [FetchDraw(primitive: .triangle, points: repeated(screen, 8), count: 24)],
        codes: uniform(8, [8, 0, 0])
    )

    /// fetch_long: 128 triangles on a 32x32 target, four slivers over each pixel row's centres; every pixel at red 4
    static let long: FetchShape = {
        var points: [SIMD4<Float>] = []
        for row in 0 ..< 32 {
            let y = Float(row) + 0.5
            for _ in 0 ..< 4 {
                points += [clip(-200, y - 0.45, side: 32), clip(-200, y + 0.45, side: 32), clip(232, y, side: 32)]
            }
        }
        return FetchShape(
            side: 32, draws: [FetchDraw(primitive: .triangle, points: points, count: 384)],
            codes: uniform(32, [4, 0, 0])
        )
    }()

    /// fetch_strip_restart: one indexed strip of three runs, back faces culled. The first run's three triangles face
    /// front; the second run's two wind opposite ways, so a strip that kept the first run's parity past the restart
    /// culls the other one of them; the third run's triangle faces back and is culled. No edge passes within 0.047 of a
    /// pixel centre (12 of the rasterizer's 1/256 steps)
    static let stripRestartDraw: FetchDraw = {
        let pixels: [SIMD2<Float>] = [
            [-1, 9], [-1, -1], [2, 9], [2, -1], [3.625, 9],
            [4.5, -1], [4.125, 9.25], [9.5, 9.25], [10.5, -1.875],
            [0, 0], [8.625, 1.375], [0.375, 8.75],
        ]
        let indices: [UInt16] = [0, 1, 2, 3, 4, restart, 5, 6, 7, 8, restart, 9, 11, 10]
        return FetchDraw(
            primitive: .triangleStrip, points: pixels.map { clip($0.x, $0.y, side: 8) }, indices: indices,
            count: indices.count
        )
    }()

    static let stripRestart = FetchShape(
        side: 8, cull: .back, draws: [stripRestartDraw], codes: reds(coverage(stripRestartDraw, side: 8, cull: .back))
    )

    /// fetch_points: 16 points on pixel (3, 5): red 16 there, 0 elsewhere
    static let points = FetchShape(
        side: 8, vertex: "fetch_count_point_vertex",
        draws: [FetchDraw(primitive: .point, points: repeated([clip(3.5, 5.5, side: 8)], 16), count: 16)],
        codes: (0 ..< 64).map { $0 == 5 * 8 + 3 ? SIMD3(16, 0, 0) : SIMD3(0, 0, 0) }
    )

    /// fetch_lines: 8 identical lines across row 2 and a line strip of 8 segments back and forth across row 5, each past
    /// both edges: red 8 on those rows, 0 elsewhere
    static let lines: FetchShape = {
        let across = [clip(-2, 2.5, side: 8), clip(10, 2.5, side: 8)]
        let strip = (0 ..< 9).map { clip($0 % 2 == 0 ? -2 : 10, 5.5, side: 8) }
        return FetchShape(
            side: 8,
            draws: [
                FetchDraw(primitive: .line, points: repeated(across, 8), count: 16),
                FetchDraw(primitive: .lineStrip, points: strip, count: 9),
            ], codes: (0 ..< 64).map { [2, 5].contains($0 / 8) ? SIMD3(8, 0, 0) : SIMD3(0, 0, 0) }
        )
    }()

    /// fetch_primitive_id: 8 full-screen triangles writing green primitive_id / 31: red 8, green 7
    static let primitiveID = FetchShape(
        side: 8, fragment: "fetch_count_primitive_fragment",
        draws: [FetchDraw(primitive: .triangle, points: repeated(screen, 8), count: 24)], codes: uniform(8, [8, 7, 0])
    )

    /// fetch_instances: 3 full-screen triangles, 4 instances, writing blue instance_id / 31: red 12, blue 3
    static let instances = FetchShape(
        side: 8, fragment: "fetch_count_instance_fragment",
        draws: [FetchDraw(primitive: .triangle, points: repeated(screen, 3), count: 9, instances: 4)],
        codes: uniform(8, [12, 0, 3])
    )

    /// fetch_vertex_id: 8 triangles from vertexStart 5, triangle k's right edge at column k + 1, so column c is at
    /// red 8 - c; the five points before vertexStart sit at the origin, where an ID counted from the draw's first vertex
    /// would take them
    static let vertexIDDraw: FetchDraw = {
        var points = [SIMD4<Float>](repeating: [0, 0, 0.5, 1], count: 5)
        for k in 0 ..< 8 {
            let x = Float(k + 1)
            points += [clip(x, -24, side: 8), clip(x, 32, side: 8), clip(-800, 4, side: 8)]
        }
        return FetchDraw(primitive: .triangle, points: points, vertexStart: 5, count: 24)
    }()

    static let vertexID = FetchShape(
        side: 8, draws: [vertexIDDraw], codes: (0 ..< 64).map { SIMD3(UInt8(8 - $0 % 8), 0, 0) }
    )

    /// fetch_indirect: the list and the strip with restarts, each drawn indirect, in a pass each
    static let indirectList = FetchShape(
        side: 8, draws: [FetchDraw(primitive: .triangle, points: repeated(screen, 8), count: 24, indirect: true)],
        codes: uniform(8, [8, 0, 0])
    )

    static let indirectStrip: FetchShape = {
        var draw = stripRestartDraw
        draw.indirect = true
        return FetchShape(side: 8, cull: .back, draws: [draw], codes: stripRestart.codes)
    }()

    /// fetch_icb, fetch_icb_indexed: one indirect command buffer of three draws over shared points: 8 full-screen
    /// triangles, the strip's first run (a 5-vertex strip) and 4 points on pixel (6, 1). Red is 8, the strip's
    /// triangles over a pixel, and 4 more on the points' pixel
    static func commands(indexed: Bool) -> FetchShape {
        let strip = Array(stripRestartDraw.points[0 ..< 5])
        let points = repeated(screen, 8) + strip + repeated([clip(6.5, 1.5, side: 8)], 4)
        let ranges: [(MTLPrimitiveType, Range<Int>)] = [
            (.triangle, 0 ..< 24), (.triangleStrip, 24 ..< 29), (.point, 29 ..< 33),
        ]
        let draws = ranges.map { primitive, range in
            indexed
                ? FetchDraw(primitive: primitive, points: points, indices: range.map(UInt16.init), count: range.count)
                : FetchDraw(primitive: primitive, points: points, vertexStart: range.lowerBound, count: range.count)
        }
        let covered = coverage(draws[1], side: 8, cull: .none)
        return FetchShape(
            side: 8, vertex: "fetch_count_point_vertex", icb: true, draws: draws,
            codes: (0 ..< 64).map { SIMD3(UInt8(8 + covered[$0] + ($0 == 1 * 8 + 6 ? 4 : 0)), 0, 0) }
        )
    }

    static let icbDraws = commands(indexed: false)
    static let icbIndexedDraws = commands(indexed: true)

    static func repeated(_ points: [SIMD4<Float>], _ times: Int) -> [SIMD4<Float>] {
        (0 ..< times).flatMap { _ in points }
    }
}
