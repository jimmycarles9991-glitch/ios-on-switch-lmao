import Foundation

/// One process's largest colour-target image since boot, as the GM20B kernel's push scan saw it (debug kernels,
/// drivers/TegraGM20B/SwitchGpuTargetPeaks.h): a multisampled target counts its pixels, not its sample grid.
public struct TargetPeak: Equatable, Sendable {
    public let owner: String
    public let width: Int
    public let height: Int
    public let samples: Int

    public init(owner: String, width: Int, height: Int, samples: Int) {
        self.owner = owner
        self.width = width
        self.height = height
        self.samples = samples
    }

    public var pixels: Int { width * height }

    public var text: String { "\(owner):\(width)x\(height)x\(samples)" }

    /// A lock screen poster's extension or PosterBoard, which snapshots them.
    public var isPoster: Bool { owner.contains("Poster") }

    /// No more pixels than the panel's and no side longer than its long side, in either orientation.
    public func fits(width panelWidth: Int, height panelHeight: Int) -> Bool {
        pixels <= panelWidth * panelHeight && max(width, height) <= max(panelWidth, panelHeight)
    }

    /// The kernel's "owner=WxHxS;..." line, largest first; an entry that does not read is left out.
    public static func parse(_ line: String) -> [TargetPeak] {
        line.split(separator: ";").compactMap { entry in
            guard let equals = entry.lastIndex(of: "=") else { return nil }
            let size = entry[entry.index(after: equals)...].split(separator: "x").compactMap { Int($0) }
            guard size.count == 3, equals != entry.startIndex else { return nil }
            return TargetPeak(owner: String(entry[..<equals]), width: size[0], height: size[1], samples: size[2])
        }
    }
}
