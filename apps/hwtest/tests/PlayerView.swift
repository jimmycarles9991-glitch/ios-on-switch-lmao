import AVFoundation
import UIKit

public final class PlayerView: UIView {
    override public class var layerClass: AnyClass { AVPlayerLayer.self }
    public var playerLayer: AVPlayerLayer { layer as! AVPlayerLayer }
}
