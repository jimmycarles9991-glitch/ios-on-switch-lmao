import HWTestCore
import XCTest

/// gpu.x1.interlock_affinity's program against tools/benchloop/sm53/affinity.py's words (whose own test checks their
/// barriers), its reading of the RGBA32Float texels, and the ticket buffer's bound.
final class InterlockAffinityTests: XCTestCase {
    /// affinity.py code(SR_VIRTID); code(SR_AFFINITY) differs only in the second S2R's register
    static let virtidWords: [UInt64] = [
        0x07ff_bc00_e440_0701, 0xf0c8_0000_01c7_0004, 0xf0c8_0000_0037_0005, 0x50b0_0000_0007_0f00,
        0x001f_8400_fc20_07e1, 0x0400_000f_fff7_0400, 0x3828_0000_0107_0401, 0x0400_000f_fff7_0502,
        0x001f_8400_fc20_07ef, 0x3828_0000_0107_0503, 0x0423_f800_0007_0000, 0x0423_f800_0007_0101,
        0x07ff_b400_fde0_07e1, 0x0423_f800_0007_0202, 0x0423_f800_0007_0303, 0xe300_0000_0007_000f,
        0x001f_8400_fc20_07e2, 0x50b0_0000_0007_0f00, 0x50b0_0000_0007_0f00, 0x50b0_0000_0007_0f00,
    ]

    func testCodeIsTheBenchLoopsAffinityProgram() {
        XCTAssertEqual(InterlockAffinity.code(second: InterlockAffinity.srVirtId), Self.virtidWords)
        var twice = Self.virtidWords
        twice[2] = 0xf0c8_0000_01c7_0005
        XCTAssertEqual(InterlockAffinity.code(second: InterlockAffinity.srAffinity), twice)
    }

    func testHeaderIsTheKnownProgramsHeader() {
        let sph = InterlockAffinity.sph
        XCTAssertEqual(sph.count, 20)
        XCTAssertEqual(sph[0], 152_674)
        XCTAssertEqual(sph[5], 0x8000_0000)
        XCTAssertEqual(sph[18], 0xf)
        XCTAssertEqual(sph.enumerated().filter { ![0, 5, 18].contains($0.offset) }.map(\.element).max(), 0)
    }

    func testBothDrawsDecode() throws {
        let experiments = try InterlockAffinity.experiments()
        XCTAssertEqual(experiments.map(\.id), ["affinity.screen", "affinity.virtid"])
        XCTAssertEqual(experiments.map(\.kind), [.fragment, .fragment])
        XCTAssertEqual(experiments.map(\.readback), [.full, .full])
        XCTAssertEqual(experiments[0].width, 1280)
        XCTAssertEqual(experiments[0].height, 720)
        XCTAssertEqual(experiments[0].texelBytes, 16)
        XCTAssertEqual(experiments[1].program?.code.count, 160)
    }

    func testTheBufferHoldsBytesZeroToThree() {
        XCTAssertEqual(InterlockAffinity.bytesReached(byte: 0), 1024)
        XCTAssertEqual(InterlockAffinity.bytesReached(byte: 3), 4096)
        XCTAssertEqual(InterlockAffinity.bytesReached(byte: 4), 5120)
        XCTAssertEqual(InterlockAffinity.largestByte(fitting: 4096), 3)
        XCTAssertEqual(InterlockAffinity.largestByte(fitting: 256 * 256 * 4), 255)
        XCTAssertEqual(InterlockAffinity.largestByte(fitting: 0), -1)
    }

    typealias Pixels = (affinity: UInt32, second: UInt32, count: Int)

    static func texels(_ pixels: [Pixels], unmarked: Int = 0) -> Data {
        var words: [UInt32] = []
        for p in pixels {
            let texel: [UInt32] = [p.affinity & 0xffff, p.affinity >> 16, p.second & 0xffff, p.second >> 16]
            for _ in 0 ..< p.count { words += texel.map { 0x3f80_0000 | $0 } }
        }
        for _ in 0 ..< unmarked { words += [0, 0x3f80_0000, 0x3f80_0000, 0x3f80_0000] }
        return words.withUnsafeBytes { Data($0) }
    }

    static func virtid(sm: UInt32, warp: UInt32) -> UInt32 { sm << 20 | warp << 8 }

    /// Two SMs, eight warps each, every pixel's affinity `affinity`
    static func control(affinity: UInt32 = 0) -> InterlockAffinity.Reading {
        let pixels = (0 ..< 16).map { i -> Pixels in (affinity, virtid(sm: UInt32(i / 8), warp: UInt32(i % 8)), 4) }
        return InterlockAffinity.read(texels(pixels))
    }

    func testReadingSplitsTheTwoRegisters() {
        let reading = InterlockAffinity.read(
            Self.texels([(0x0000_0102, 0x0000_0102, 3), (0x0001_0000, 0x0001_0001, 2)], unmarked: 1)
        )
        XCTAssertEqual(reading.pixels, 6)
        XCTAssertEqual(reading.unmarked, 1)
        XCTAssertEqual(reading.affinity, [0x102: 3, 0x1_0000: 2])
        XCTAssertEqual(reading.lowBytes, [0x00, 0x02])
        XCTAssertEqual(reading.secondDiffers, 2)
        let control = Self.control(affinity: 1)
        XCTAssertEqual(control.sms, [0: 32, 1: 32])
        XCTAssertEqual(control.warps, 8)
        XCTAssertEqual(control.affinityBySm, [0: [1], 1: [1]])
    }

    func testOneGpcOnTwoSmsPasses() {
        let screen = InterlockAffinity.read(Self.texels([(0, 0, 100)]))
        let finding = InterlockAffinity.judge(
            screen: .success(screen), control: .success(Self.control()), tpcs: 2, ticketBytes: 4096
        )
        XCTAssertEqual(finding.verdict, .pass, finding.note)
        XCTAssertEqual(finding.metrics["affinity_low_max"], .integer(0))
        XCTAssertEqual(finding.metrics["affinity_values"], .text("0x0:100"))
        XCTAssertEqual(finding.metrics["sm_ids"], .text("0x0:32,0x1:32"))
        XCTAssertEqual(finding.metrics["byte_limit"], .integer(3))
    }

    func testAByteBeyondTheBufferFailsNamingIt() {
        let screen = InterlockAffinity.read(Self.texels([(0, 0, 90), (0x0000_0104, 0x0000_0104, 10)]))
        let finding = InterlockAffinity.judge(
            screen: .success(screen), control: .success(Self.control()), tpcs: 2, ticketBytes: 4096
        )
        XCTAssertEqual(finding.verdict, .fail)
        XCTAssertTrue(finding.note.hasPrefix("affinity byte 0x4 reaches 5120 bytes of the 4096-byte"), finding.note)
        XCTAssertTrue(finding.note.contains("gpu.cpp"), finding.note)
        XCTAssertEqual(finding.metrics["affinity_low_max"], .integer(4))
    }

    func testTheSameByteFitsAGrownBuffer() {
        let screen = InterlockAffinity.read(Self.texels([(4, 4, 10)]))
        let finding = InterlockAffinity.judge(
            screen: .success(screen), control: .success(Self.control(affinity: 4)), tpcs: 2, ticketBytes: 256 * 1024
        )
        XCTAssertEqual(finding.verdict, .pass, finding.note)
    }

    func testNoControlLeavesItInconclusive() {
        let screen = InterlockAffinity.read(Self.texels([(0, 0, 100)]))
        let trapped = InterlockAffinity.judge(
            screen: .success(screen), control: .failure(TestFailure("killed e2: trap")), tpcs: 2, ticketBytes: 4096
        )
        XCTAssertEqual(trapped.verdict, .inconclusive)
        XCTAssertTrue(trapped.note.contains("control draw: killed e2"), trapped.note)
        let oneSm = InterlockAffinity.read(Self.texels([(0, Self.virtid(sm: 0, warp: 1), 5), (0, 0, 5)]))
        let finding = InterlockAffinity.judge(
            screen: .success(screen), control: .success(oneSm), tpcs: 2, ticketBytes: 4096
        )
        XCTAssertEqual(finding.verdict, .inconclusive)
        XCTAssertTrue(finding.note.contains("1 SM ids where GetInfo counts 2"), finding.note)
    }

    func testAffinityVaryingOnTheScreenIsAControlToo() {
        let screen = InterlockAffinity.read(Self.texels([(0, 0, 50), (0x100, 0x100, 50)]))
        let finding = InterlockAffinity.judge(
            screen: .success(screen), control: .failure(TestFailure("killed e2: trap")), tpcs: 2, ticketBytes: 4096
        )
        XCTAssertEqual(finding.verdict, .pass, finding.note)
        XCTAssertEqual(finding.metrics["control"], .text("affinity_varies"))
        XCTAssertEqual(finding.note, "control draw: killed e2: trap")
    }

    func testTheControlKeepsSrVirtIdsWholeValues() {
        let finding = InterlockAffinity.judge(
            screen: .failure(TestFailure("killed e2")), control: .success(Self.control()), tpcs: 2, ticketBytes: 4096
        )
        XCTAssertEqual(finding.verdict, .inconclusive)
        XCTAssertEqual(finding.metrics["control"], .text("virtid"))
        let lowWarps = (0 ..< 8).map { "0x" + String($0 << 8, radix: 16) + ":4" }.joined(separator: ",")
        XCTAssertEqual(finding.metrics["virtid_values"], .text(lowWarps + ",+8"))
        XCTAssertTrue(finding.note.hasPrefix("affinity draw: killed e2"), finding.note)
    }

    func testUnmarkedPixelsLeaveItInconclusive() {
        let screen = InterlockAffinity.read(Self.texels([(0, 0, 100)], unmarked: 3))
        let finding = InterlockAffinity.judge(
            screen: .success(screen), control: .success(Self.control()), tpcs: 2, ticketBytes: 4096
        )
        XCTAssertEqual(finding.verdict, .inconclusive)
        XCTAssertEqual(finding.metrics["unmarked_px"], .integer(3))
    }
}
