import HWTestCommon
import HWTestCore
import HWTestKit
import HWTestShared
import UIKit

enum PacingTests {
    static let all: [any HWTest] = [metal, text, blurOver, blurUnder, list]

    static let metal = PacingTest(
        id: "pacing.metal_layer", name: "Pacing: a full-screen Metal layer alone", probeSize: nil
    ) { _, _ in { _ in } }

    static let text = PacingTest(id: "pacing.text", name: "Pacing: labels redrawn every frame", probeSize: 48) {
        stage, screen in
        let labels = UITextTest.styles.enumerated().map { line, style -> UILabel in
            let label = UILabel(frame: CGRect(x: 10, y: 10 + CGFloat(line) * 38, width: screen.width - 80, height: 36))
            label.font = UIFont.preferredFont(forTextStyle: style)
            stage.addSubview(label)
            return label
        }
        stage.backgroundColor = .white
        return { frame in for (line, label) in labels.enumerated() { label.text = "Wi-Fi \(frame * (line + 1))" } }
    }

    /// r45-render's (d): a full-screen layer animated over a blurred (still) backdrop.
    static let blurOver = PacingTest(
        id: "pacing.blur_over", name: "Pacing: a full-screen layer animated over a blur", probeSize: 48
    ) { stage, screen in
        stage.addSubview(BackdropStripes(frame: screen))
        let blur = UIVisualEffectView(effect: UIBlurEffect(style: .regular))
        blur.frame = screen
        stage.addSubview(blur)
        let veil = UIView(frame: screen)
        stage.addSubview(veil)
        return { frame in
            veil.backgroundColor = UIColor(
                hue: CGFloat(frame % 120) / 120, saturation: 0.7, brightness: 0.9, alpha: 0.35
            )
            veil.transform = CGAffineTransform(translationX: CGFloat(frame % 40) - 20, y: 0)
        }
    }

    static let blurUnder = PacingTest(
        id: "pacing.blur_under", name: "Pacing: a backdrop moving under a full-screen blur", probeSize: 48
    ) { stage, screen in
        let stripes = BackdropStripes(frame: screen.insetBy(dx: -64, dy: 0))
        stage.addSubview(stripes)
        let blur = UIVisualEffectView(effect: UIBlurEffect(style: .regular))
        blur.frame = screen
        stage.addSubview(blur)
        return { frame in stripes.transform = CGAffineTransform(translationX: CGFloat(frame % 32) * 2 - 32, y: 0) }
    }

    static let list = PacingTest(id: "pacing.list", name: "Pacing: a list scrolled every frame", probeSize: 48) {
        stage, screen in
        let table = SettingsList.make(screen)
        stage.addSubview(table)
        return { frame in table.contentOffset = CGPoint(x: 0, y: CGFloat(frame % 300) * 4) }
    }
}
