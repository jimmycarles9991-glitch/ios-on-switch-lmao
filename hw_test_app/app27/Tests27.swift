import HWTestGPU
import HWTestKit

/// What only iOS 27 has: QuartzCore 27's glass states on the raw path, Metal 4, Liquid Glass through UIKit and the
/// render server, True Tone's default, and VideoToolbox's frame processors. The frame processors run after the rest:
/// they may reach hardware J181 has and the Switch lacks. The X1 hardware probes come last, the risky ones at the end.
/// ui.glass_ab runs before every other UIKit glass test: its first round is then the first glass stage a test built.
enum Tests27 {
    static let all: [any HWTest] =
        [
            GPUSceneTest(scene: Scenes.glass), CAGlassSplitTest(), GPUSceneTest(scene: CAFetchRouteScene()),
            Metal4Test(), BinaryArchiveTest(), UIGlassABTest(), UIGlassTest(), UIGlassMotionTest(),
            UIGlassFirstFrameTest(), UIGlassLiveTest(), UIGlassSidebarTest(), UIGlassSettingsTest(),
            PocketStripTest(), PocketKeylineTest(),
            TrueToneDefaultTest(), GlassPacing.over,
            GlassPacing.under, NoCameraTest(), FrameProcessorTest(),
        ] + X1Probes.all
}
