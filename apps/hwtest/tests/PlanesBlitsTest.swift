import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal
import OSLog

/// Blits of textures held through planes ('420v' 500, 'x420' 505, 'p420' 508, '2vuy' 501, 'b3a8' 550 and the one-plane
/// 504 and 524 over 'v410', 562 over '2vuy') complete on the driver and move exactly the bytes their rule names, every
/// other byte of each surface (to its last page's end), buffer and plain texture as it was (PlanesBlitCases.swift): A a
/// buffer into plane 0, B plane 0 into a buffer (24x8 surfaces: the region (6,2) 12x3 runs past 16), C plane 0 into a
/// plain texture as one run in its texel, through row padding, plane 1 and the last page's tail (also at an exact
/// pitch, so no padding answers for it), D a 16x8 plain texture into plane 0 where its texel is plane 0's unit, E
/// planes into planes, F a colour fill, G a run from a surface of exactly one page, within it and past it.
///
/// A to E and G within the page are an M4 Max's bytes under macOS 27.2.1, validation off (probes blitgroups.mm,
/// bound.mm, units.mm; E and D's outside bytes this test's own cases on it, three runs): E moves plane 1 between two
/// with one a byte a texel at plane 0's rows & ~15 for the region's rows, on through plane 1's end (a source with no
/// plane 1 is gpu.gm20b.planes_copies_open's). D of another texel size reads the plain texture's own memory layout,
/// Apple's not GM20B's: its 8u x 4 bytes are reported (<case>_unjudged=, unjudged_bytes=), the rest judged. G past the
/// page (Apple reads zeros there) expects nothing written and gm20b:blit.invalid; F (Apple writes plane 0 with a value
/// that ignores the colour) nothing and gm20b:blit.undefined. Every copy across two pixel formats (C, D, E's), fill (F)
/// and G is counted skipped (skipped.call.blit, skipped_expected=). Each rule's line is logged once a process
/// (blit.invalid once per call and rule), so the process's log must hold those lines and none of them twice afterwards
/// (invalid_lines=, invalid_repeats=, undefined_lines=, undefined_repeats=). cases= and cases_off=; for a case off its
/// rule or the driver only (a line of every case would pass os_log's limit), <case>_changed= (bytes off the rule) and
/// <case>_path=; failed_cbs=.
struct PlanesBlitsTest: HWTest {
    let id = "gpu.gm20b.planes_blits"
    let name = "Blits of textures held through YCbCr and RGB10A8_2P_XR10 planes move only their rule's bytes, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var off = 0, unjudged = 0
        let cases = PlanesBlitCases.all
        for (key, _, run) in cases {
            do {
                let result = try await run(gpu)
                unjudged += result.unjudged
                if result.unjudged != 0 { metrics.set("\(key)_unjudged", result.unjudged) }
                if result.changed != 0 || result.path != "gm20b" {
                    off += 1
                    metrics.set("\(key)_changed", result.changed)
                    metrics.set("\(key)_path", String(result.path.prefix(60)))
                }
                checks.append(("\(key): on the driver", result.path == "gm20b"))
                checks.append(("\(key): \(result.changed) bytes off its rule (\(result.text))", result.changed == 0))
            } catch {
                off += 1
                checks.append(("\(key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("cases", cases.count)
        metrics.set("cases_off", off)
        metrics.set("unjudged_bytes", unjudged)
        for (field, reason) in [("invalid", "blit.invalid"), ("undefined", "blit.undefined")] {
            let lines = Self.refusalLines(reason)
            let repeats = lines.map { $0.count - Set($0).count }
            metrics.set(field + "_lines", lines?.count ?? -1)
            metrics.set(field + "_repeats", repeats ?? -1)
            let seen = lines.map { "\($0.count) lines, \($0.count - Set($0).count) repeated" } ?? "no log"
            checks.append(
                ("gm20b:\(reason) logged, no rule's line twice (\(seen))", (lines?.count ?? 0) > 0 && repeats == 0)
            )
        }
        let counted = GM20BFacade.counters(of: gpu.device)
        let skips = cases.reduce(0) { $0 + $1.skips }
        metrics.set("skipped_expected", skips)
        checks.append(FailedCounts(before: before, after: counted).skipCheck(expecting: ["call.blit": skips]))
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// The process's raw:<reason> lines (kits/SwitchRefusal: subsystem com.brunostjohn.switch.refusal, category raw,
    /// "<process>[<pid>] <reason>: <text>"); nil when its log cannot be read
    static func refusalLines(_ reason: String) -> [String]? {
        let subsystem = "com.brunostjohn.switch.refusal", text = " \(reason): "
        guard let store = try? OSLogStore(scope: .currentProcessIdentifier),
              let entries = try? store.getEntries(
                  matching: NSPredicate(
                      format: "subsystem == %@ AND category == %@ AND composedMessage CONTAINS %@", subsystem, "gm20b",
                      text
                  )
              )
        else { return nil }
        return entries.compactMap { $0 as? OSLogEntryLog }.filter {
            $0.subsystem == subsystem && $0.category == "gm20b" && $0.composedMessage.contains(text)
        }.map(\.composedMessage)
    }
}
