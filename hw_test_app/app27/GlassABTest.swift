import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import UIKit

/// ui.glass_first's black first frame under one change at a time, each arm's rounds interleaved with control rounds
/// (the same round unchanged). A round's snapshot is encoding A when the render server's command buffers for it render
/// into mip levels (Core Animation regenerated the backdrop pyramid) and B when they render none (it reused the last
/// one). Arms:
///   nudge   the stripes move a point just before the snapshot: does B become A and the glass good?
///   zero    every new buffer in every process starts zeroed (debug switch zero-new-buffers): state left in reused
///           memory?
///   whole   the render server draws whole programs, no split prologue (debug switch whole-programs)
///   wfi     the render server's fetch barrier waits for idle (debug switch fetch-barrier-wfi)
///   dirty   the render server first renders and drops a screen of glass and blur over noise, leaving float
///           intermediates' memory behind in its pool
/// first_in_process= is the first control round's verdict when no test had built a glass stage before it (the render
/// server still holds SpringBoard's and this app's own glass). Every snapshot arms the glass probe's draw lines (`raw:
/// glass draw g<generation> ...` in every process, `raw: glass cb g<generation>` in the render server):
/// Documents/ui/ui.glass_ab/rounds.json lists each round's arm, verdict, encoding, command buffers and generation;
/// black pictures go beside it. The render server's draw and record lines share the generation; another process's draw
/// lines count only its own armings.
struct UIGlassABTest: HWTest {
    let id = "ui.glass_ab"
    let name = "UIGlassEffect's first frame: what each change does to the black glass"
    let subsystem = Subsystem.compositor
    var timeout: TimeInterval { 180 }
    static let rounds = 3

    enum Arm: String, CaseIterable {
        case nudge, zero, whole, wfi, dirty
    }

    struct Entry: Encodable {
        let arm: String
        let control: Bool
        let black: Bool
        let level: Int
        let encoding: String
        let buffers: String
        let lost: UInt64
        let generation: UInt64
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let directory = context.documents.appendingPathComponent("ui").appendingPathComponent(id)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var entries: [Entry] = []
        var metrics = Metrics()
        var firstInProcess: String?
        for arm in Arm.allCases {
            var (armed, control) = ([GlassRound](), [GlassRound]())
            for _ in 0..<Self.rounds {
                let plain = await GlassRound.run(context, lines: true)
                if firstInProcess == nil {
                    firstInProcess = plain.firstInProcess ? (plain.black ? "black" : "good") : "-"
                }
                control.append(plain)
                armed.append(await Self.round(arm, context))
            }
            for (round, isControl) in armed.map({ ($0, false) }) + control.map({ ($0, true) }) {
                entries.append(
                    Entry(
                        arm: arm.rawValue, control: isControl, black: round.black,
                        level: Int((round.level / 3 * 255).rounded()), encoding: round.recording.encoding,
                        buffers: round.recording.signature, lost: round.recording.lost,
                        generation: round.recording.generation))
                if round.black {
                    let name = "\(arm.rawValue)-\(isControl ? "control" : "arm")-\(entries.count).png"
                    try? round.picture.png()?.write(to: directory.appendingPathComponent(name))
                }
            }
            metrics.set(arm.rawValue, Self.field(armed) + "|c" + Self.field(control))
        }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted]
        try? encoder.encode(entries).write(to: directory.appendingPathComponent("rounds.json"))
        var line = Metrics()
        line.set("arm_black", entries.filter { !$0.control && $0.black }.count)
        line.set("control_black", entries.filter { $0.control && $0.black }.count)
        line.set("first_in_process", firstInProcess ?? "-")
        line.merge(metrics)
        let blackA = entries.filter { $0.black && $0.encoding == "A" }.count
        let blackB = entries.filter { $0.black && $0.encoding == "B" }.count
        line.set("black_by_encoding", "A\(blackA)/B\(blackB)")
        let black = entries.filter(\.black).count
        return black == 0
            ? .pass(line)
            : .fail("\(black) of \(entries.count) rounds show the glass over the grey black", line)
    }

    /// "<black>/<rounds>:<encodings>", e.g. "3/3:BBB"
    static func field(_ rounds: [GlassRound]) -> String {
        "\(rounds.filter(\.black).count)/\(rounds.count):" + rounds.map(\.recording.encoding).joined()
    }

    static func round(_ arm: Arm, _ context: TestContext) async -> GlassRound {
        switch arm {
        case .nudge:
            return await GlassRound.run(context, lines: true) { stage in
                stage.stripes.transform = CGAffineTransform(translationX: 1, y: 0)
            }
        case .zero: return await switched("zero-new-buffers", context)
        case .whole: return await switched("whole-programs", context)
        case .wfi:
            GlassProbe.post("fetch-barrier-wfi")
            let round = await GlassRound.run(context, lines: true)
            GlassProbe.post("fetch-barrier-ps")
            return round
        case .dirty:
            await GlassDirt.render(context)
            return await GlassRound.run(context, lines: true)
        }
    }

    static func switched(_ name: String, _ context: TestContext) async -> GlassRound {
        GlassProbe.post(name + "-on")
        let round = await GlassRound.run(context, lines: true)
        GlassProbe.post(name + "-off")
        return round
    }
}

/// A screen the render server draws through float intermediates for a second and then drops: noise under glass and blur
/// panes that move, so its pool holds what they left when the next stage's intermediates are made.
enum GlassDirt {
    static func render(_ context: TestContext) async {
        let panes = await context.onStage(fullScreen: true) { stage -> [UIView] in
            let screen = UIScreen.main.bounds
            var generator = SystemRandomNumberGenerator()
            for _ in 0..<160 {
                let block = UIView(
                    frame: CGRect(
                        x: CGFloat.random(in: 0..<screen.width, using: &generator),
                        y: CGFloat.random(in: 0..<screen.height, using: &generator), width: 48, height: 48))
                block.backgroundColor = UIColor(
                    hue: CGFloat.random(in: 0..<1, using: &generator), saturation: 1, brightness: 1, alpha: 1)
                stage.addSubview(block)
            }
            return (0..<6).map { i -> UIView in
                let effect: UIVisualEffect =
                    i % 2 == 0 ? UIGlassEffect(style: .regular) : UIBlurEffect(style: .systemThinMaterial)
                let pane = UIVisualEffectView(effect: effect)
                pane.frame = CGRect(
                    x: CGFloat(i % 3) * screen.width / 3, y: CGFloat(i / 3) * screen.height / 2,
                    width: screen.width / 3, height: screen.height / 2)
                stage.addSubview(pane)
                return pane
            }
        }
        for step in 0..<30 {
            await MainActor.run {
                for (i, pane) in panes.enumerated() {
                    pane.transform = CGAffineTransform(translationX: CGFloat((step + i) % 8) * 3, y: 0)
                }
            }
            try? await Task.sleep(nanoseconds: 33_000_000)
        }
        await context.clearStage()
    }
}
