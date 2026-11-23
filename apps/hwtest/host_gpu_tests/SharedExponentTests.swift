import Foundation
import HWTestGPU
import XCTest

final class SharedExponentTests: XCTestCase {
    /// MSL 8.7.5's float to RGB9E5 conversion, N = 9, B = 15, Emax = 31
    private func msl(_ rgb: [Double]) -> UInt32 {
        let n = 9.0, b = 15.0, emax = 31.0
        let largest = (pow(2, n) - 1) / pow(2, n) * pow(2, emax - b)
        let clamped = rgb.map { max(0, min(largest, $0)) }
        let maxc = clamped.max()!
        let preliminary = max(-b - 1, floor(log2(maxc))) + 1 + b
        let exponent =
            floor(maxc / pow(2, preliminary - b - n) + 0.5) == pow(2, n) ? preliminary + 1 : preliminary
        let m = clamped.map { UInt32(floor($0 / pow(2, exponent - b - n) + 0.5)) }
        return m[0] | m[1] << 9 | m[2] << 18 | UInt32(exponent) << 27
    }

    private func value(_ word: UInt32) -> [Double] {
        let scale = pow(2, Double(Int(word >> 27) - 15 - 9))
        return [word & 0x1FF, word >> 9 & 0x1FF, word >> 18 & 0x1FF].map { Double($0) * scale }
    }

    func testEncodingIsMSLsEncodingOfTheWordsValue() {
        let mantissas: [UInt32] = [0, 1, 2, 3, 127, 128, 129, 255, 256, 257, 300, 511]
        for exponent in UInt32(0) ... 31 {
            for r in mantissas {
                for g in mantissas {
                    for b in mantissas {
                        let word = r | g << 9 | b << 18 | exponent << 27
                        XCTAssertEqual(SharedExponent.encoding(word), msl(value(word)), String(word, radix: 16))
                        XCTAssertEqual(value(SharedExponent.encoding(word)), value(word))
                    }
                }
            }
        }
    }

    /// An Apple M4 Max's blit read-back of a word gpu.heap.kinds wrote (exponent 3, largest mantissa 133)
    func testEncodingIsWhatAppleReadsBack() {
        XCTAssertEqual(SharedExponent.encoding(0x1811_0A03), 0x1022_1406)
        XCTAssertEqual(SharedExponent.encoding(0x1022_1406), 0x1022_1406)
        XCTAssertEqual(SharedExponent.encoding(31 << 27), 0)
        XCTAssertEqual(SharedExponent.encodings([0x03, 0x0A, 0x11, 0x18]), [0x06, 0x14, 0x22, 0x10])
    }
}
