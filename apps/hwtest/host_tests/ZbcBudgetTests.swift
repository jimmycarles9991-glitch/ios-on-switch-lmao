import HWTestCore
import XCTest

/// 25 Nov 19:18: the boot's 6 colour entries and one of another process's left 8 slots for compressed_targets' 9
/// values, and its last arm (rgba32f) was refused.
final class ZbcBudgetTests: XCTestCase {
    static let codes = [51.0 / 255, 102.0 / 255, 204.0 / 255, 1]
    static let cases = [
        ZbcValue.unorm("ms4_bgra8", format: 0x18, codes, bits: [8, 8, 8, 8]),
        ZbcValue.unorm("ms4_rgba8", format: 0x28, codes, bits: [8, 8, 8, 8]),
        ZbcValue.half("ms4_f16", format: 0x16, [0.75, 0, 0, 0]),
        ZbcValue.half("ms4_f16_bgra8_floats", format: 0x16, codes),
    ]
    static let arms = [
        ZbcValue(name: "bgra8srgb", format: 0x1c, channels: [0.033, 0.133, 0.604, 1]),
        ZbcValue(name: "rgba8srgb", format: 0x2c, channels: [0.033, 0.133, 0.604, 1]),
        ZbcValue.unorm(
            "rgb10a2",
            format: 0x20,
            [300.0 / 1023, 600.0 / 1023, 900.0 / 1023, 2.0 / 3],
            bits: [10, 10, 10, 2]
        ),
        ZbcValue.unorm(
            "rgba16unorm",
            format: 0x08,
            [10000.0 / 65535, 20000.0 / 65535, 40000.0 / 65535, 1],
            bits: [16, 16, 16, 16]
        ),
        ZbcValue.float("rgba32f", format: 0x04, [0.3, -1.5, 7.25, 0.5]),
    ]

    static func line(_ entries: [ZbcValue], slots: Int = 15) -> String {
        let colours = entries.map { value in
            String(value.format, radix: 16) + ":"
                + value.channels.map { String($0.bitPattern, radix: 16) }.joined(separator: ",") + ";"
        }
        return "colour=\(entries.count)/\(slots) depth=2/15 c=\(colours.joined()) d=1:0;1:3f800000;"
    }

    static let boot = [
        ZbcValue(name: "", format: 0x01, channels: [0, 0, 0, 0]),
        ZbcValue(name: "", format: 0x02, channels: [1, 1, 1, 1]),
        ZbcValue(name: "", format: 0x04, channels: [0, 0, 0, 0]),
        ZbcValue(name: "", format: 0x04, channels: [1, 1, 1, 1]),
        ZbcValue(name: "", format: 0x28, channels: [0, 0, 0, 1]),
        ZbcValue(name: "", format: 0x18, channels: [0, 0, 0, 1]),
    ]

    func testTheKernelLineReadsBack() throws {
        let table = try XCTUnwrap(ZbcTable.parse(Self.line(Self.boot + [Self.arms[4]])))
        XCTAssertEqual(table.used, 7)
        XCTAssertEqual(table.slots, 15)
        XCTAssertEqual(table.free, 8)
        XCTAssertTrue(table.holds(Self.arms[4]))
        XCTAssertFalse(table.holds(Self.arms[3]))
        XCTAssertNil(ZbcTable.parse("lines=3"), "another op's line")
        XCTAssertEqual(ZbcTable.parse("colour=6/15 depth=2/15 c= d=")?.colours, [])
    }

    func testAFreshBootFitsEveryValue() throws {
        let budget = ZbcBudget(
            try XCTUnwrap(ZbcTable.parse(Self.line(Self.boot))),
            required: Self.cases,
            optional: Self.arms
        )
        XCTAssertEqual(budget.required, 4)
        XCTAssertTrue(budget.requiredFits)
        XCTAssertEqual(budget.run, Self.arms.map(\.name))
        XCTAssertEqual(budget.noRoom, [])
        XCTAssertNil(budget.refusal(boot: 6))
    }

    func testOneForeignEntryLeavesTheLastArmWithoutRoomInsteadOfRefused() throws {
        let foreign = ZbcValue(name: "", format: 0x18, channels: [0.5, 0.5, 0.5, 1])
        let table = try XCTUnwrap(ZbcTable.parse(Self.line(Self.boot + [foreign])))
        let budget = ZbcBudget(table, required: Self.cases, optional: Self.arms)
        XCTAssertTrue(budget.requiredFits)
        XCTAssertEqual(budget.run, ["bgra8srgb", "rgba8srgb", "rgb10a2", "rgba16unorm"])
        XCTAssertEqual(budget.noRoom, ["rgba32f"])
    }

    func testARerunInTheSameBootNeedsNoRoomForItsOwnEntries() throws {
        let table = try XCTUnwrap(ZbcTable.parse(Self.line(Self.boot + Self.cases + Self.arms)))
        XCTAssertEqual(table.free, 0)
        let budget = ZbcBudget(table, required: Self.cases, optional: Self.arms)
        XCTAssertEqual(budget.required, 0)
        XCTAssertTrue(budget.requiredFits)
        XCTAssertEqual(budget.run.count, 5)
    }

    func testAValueTwoCasesShareTakesOneSlot() throws {
        let shared = ZbcValue.half("again", format: 0x16, [0.75, 0, 0, 0])
        let budget = ZbcBudget(
            try XCTUnwrap(ZbcTable.parse(Self.line(Self.boot))), required: Self.cases + [shared], optional: []
        )
        XCTAssertEqual(budget.required, 4)
    }

    func testAFullTableFailsTheRequiredValuesWithItsCounts() throws {
        let others = (0 ..< 9).map { ZbcValue(name: "", format: 0x18, channels: [Float($0) / 10, 0, 0, 1]) }
        let budget = ZbcBudget(
            try XCTUnwrap(ZbcTable.parse(Self.line(Self.boot + others))), required: Self.cases, optional: Self.arms
        )
        XCTAssertFalse(budget.requiredFits)
        XCTAssertEqual(budget.noRoom.count, 5)
        let why = try XCTUnwrap(budget.refusal(boot: 6))
        XCTAssertTrue(why.contains("0 free slots"), why)
        XCTAssertTrue(why.contains("needs 4 new entries"), why)
        XCTAssertTrue(why.contains("9 entries past the boot's 6"), why)
    }

    func testFloatRoundingDoesNotHideAHeldValue() {
        let held = ZbcValue(name: "", format: 0x1c, channels: [0.0331047, 0.1328683, 0.6038273, 1])
        let asked = ZbcValue(name: "x", format: 0x1c, channels: [0.03310477, 0.13286833, 0.60382736, 1])
        XCTAssertTrue(ZbcTable(used: 7, slots: 15, colours: [held]).holds(asked))
        XCTAssertFalse(ZbcTable(used: 7, slots: 15, colours: [held]).holds(ZbcValue(
            name: "x",
            format: 0x2c,
            channels: asked.channels
        )))
    }
}
