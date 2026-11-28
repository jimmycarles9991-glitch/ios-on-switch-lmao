import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import Metal
import XCTest

/// Which rows a quarter-turned pass over overlapping plain rows keeps, on this Mac's GPU, for one- and two-column
/// frames (gpu.gm20b.plain_rows_quarter_turns): each element several rows reach and every run keeps the same row of,
/// scored against fixed store orders of the frame's tiles (overlap_order.h frameOrder's family). The line per shape
/// names the elements the runs agree on and how many each order predicts; the order that predicts all of them in every
/// shape is the one the driver can take.
final class PlainRowsTurnOrderTests: XCTestCase {
    struct Order: CustomStringConvertible {
        let tileW, tileH: Int
        let columnMajor: Bool
        let rightLater, downLater: Bool

        var description: String {
            "\(tileW)x\(tileH) \(columnMajor ? "col" : "row") \(rightLater ? "right" : "left") "
                + "\(downLater ? "down" : "up")"
        }

        func key(_ fx: Int, _ fy: Int) -> Int {
            let column = rightLater ? fx / tileW : 0x3ff - fx / tileW
            let row = downLater ? fy / tileH : 0x3ff - fy / tileH
            let inTile = (fx % 64 / 32) << 9 | (fy % 32 / 2) << 5 | (fx % 32 / 2) << 1 | (fy % 2 == 0 ? 1 : 0)
            return (columnMajor ? column << 10 | row : row << 10 | column) << 12 | inTile
        }

        static let all: [Order] = [32, 64].flatMap { w in
            [32, 64].flatMap { h in
                [true, false].flatMap { c in
                    [true, false].flatMap { r in [true, false].map { d in Order(
                        tileW: w,
                        tileH: h,
                        columnMajor: c,
                        rightLater: r,
                        downLater: d
                    ) } }
                }
            }
        }
        static let driver = Order(tileW: 64, tileH: 32, columnMajor: true, rightLater: false, downLater: true)
    }

    private func makeGPU() throws -> GPU {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        var source = ""
        for file in ["HWTest.metal", "PlainRows.metal"] {
            source += try String(contentsOfFile: directory + "/" + file, encoding: .utf8) + "\n"
        }
        let library = try device.makeLibrary(source: source, options: MTLCompileOptions())
        return GPU(device: device, queue: queue, library: library)
    }

    /// Frame texel of plane texel (x, y) under `turn` (PlainRows.metal plain_rows_texel's inverse)
    static func frame(_ s: PlainRows.Shape, _ x: Int, _ y: Int) -> (Int, Int) {
        s.turn == 1 ? (y, s.width - 1 - x) : (s.height - 1 - y, x)
    }

    func testQuarterTurnedPassesKeepRowsInOneOrder() async throws {
        let gpu = try makeGPU()
        let runs = 8
        var fitsEverywhere = Set(Order.all.indices)
        for (width, height) in [(40, 16), (40, 24), (40, 40), (72, 40), (100, 48), (40, 64), (72, 72), (40, 100),
                                (100, 100), (136, 100)] {
            for turn in [1, 3] {
                var s = PlainRows.Shape(MTLPixelFormat.rgba32Uint.rawValue, width, height, bpp: 16, pitch: 128)
                s.turn = turn
                var holders: [[Int]] = []
                for _ in 0 ..< runs {
                    let stored = try await PlainRows.stored(gpu, s)
                    guard let bytes = stored.bytes else {
                        XCTFail("no texture over rows of \(s.pitch) at turn \(turn)")
                        return
                    }
                    holders.append(PlainRows.holders(s, bytes))
                }
                var agreed: [(a: Int, row: Int)] = []
                for (i, a) in Swift.stride(from: 0, to: s.span, by: s.bpp).enumerated()
                    where s.rows(reaching: a).count > 1 {
                    let row = holders[0][i]
                    if row >= 0, holders.allSatisfy({ $0[i] == row }) { agreed.append((a, row)) }
                }
                var hits: [Int] = []
                for order in Order.all {
                    hits.append(agreed.filter { e in
                        let rows = s.rows(reaching: e.a)
                        let winner = rows.max { r0, r1 in
                            let f0 = Self.frame(s, (e.a - s.stride * r0) / s.bpp, r0)
                            let f1 = Self.frame(s, (e.a - s.stride * r1) / s.bpp, r1)
                            return order.key(f0.0, f0.1) < order.key(f1.0, f1.1)
                        }
                        return winner == e.row
                    }.count)
                }
                let best = hits.max() ?? 0
                for i in Order.all.indices where hits[i] != agreed.count { fitsEverywhere.remove(i) }
                let driver = Order.all.firstIndex { $0.description == Order.driver.description }!
                let leaders = Order.all.indices.filter { hits[$0] == best }.map { Order.all[$0].description }
                print(
                    "quarter_turn \(width)x\(height) turn \(turn): agreed \(agreed.count), driver \(hits[driver]), "
                        + "best \(best): \(leaders.prefix(4).joined(separator: "; "))"
                )
            }
        }
        print("quarter_turn orders that fit every agreed element of every shape: "
            +
            (fitsEverywhere.isEmpty ? "none" : fitsEverywhere.map { Order.all[$0].description }
                .joined(separator: "; ")))
    }
}
