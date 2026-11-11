import Foundation
import HWTestCore
import UIKit

/// An asset catalog image compiled GPU-compressed (assets/Compressed.xcassets, ASTC): CoreUI hands Core Animation its blocks,
/// the render server lays them out through the device's texture layout and samples a no-copy texture over that memory
/// (drivers/TegraGM20BMetal/raw/runtime/RawMetalNoCopy.mm). Shown at one texel a pixel, its composite has to match the
/// CPU's drawing of the same image and must not be blank (a nil texture draws nothing).
struct CompressedImageTest: HWTest {
    let id = "ui.compressed_image"
    let name = "GPU-compressed asset catalog image, composited against the CPU"
    let subsystem = Subsystem.compositor

    func run(_ context: TestContext) async throws -> Outcome {
        guard let image = await MainActor.run(body: { UIImage(named: "ASTCPattern", in: .main, with: nil) }) else {
            return .fail("ASTCPattern is not in the app's asset catalog", Metrics())
        }
        let check = await UISceneTests.staged(context) { stage, _ -> UIImageView in
            let view = UIImageView(image: image)
            let scale = UIScreen.main.scale
            view.frame = Snapshots.aligned(
                CGRect(
                    x: 40, y: 40, width: image.size.width * image.scale / scale,
                    height: image.size.height * image.scale / scale))
            stage.addSubview(view)
            return view
        } _: { view in
            CompositeCheck(label: "image", composited: Snapshots.composited(view), cpu: Snapshots.cpu(view))
        }
        var metrics = Metrics()
        let level = DisplaySnapshotTest.mean(check.composited)
        metrics.set("mean", DisplaySnapshotTest.level(level))
        if level < 0.05 {
            return .fail("the composited image is blank (mean \(DisplaySnapshotTest.level(level)))", metrics)
        }
        return UISceneTests.outcome([("image", check)], context, id: id, metrics: metrics)
    }
}
