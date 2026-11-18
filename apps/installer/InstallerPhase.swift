public enum InstallerPhase: Equatable {
    case idle
    case importing
    case downloading(Int64, Int64)
    case checking
    case ready
    case installing
    case installed
    case failed(String, retryInstallation: Bool)

    public var isBusy: Bool {
        switch self {
        case .importing, .downloading, .checking, .installing: true
        default: false
        }
    }

    public var canCancel: Bool {
        switch self {
        case .importing, .downloading, .checking: true
        default: false
        }
    }

    public var canInstall: Bool {
        switch self {
        case .ready: true
        case .failed(_, let retry): retry
        default: false
        }
    }
}

public struct InstallerProgress: Equatable {
    public private(set) var title: String
    public private(set) var percent: Int?

    public init() {
        title = "Starting installation…"
    }

    public mutating func update(status: String?, percent reported: Double?) {
        if let reported, reported.isFinite, (0 ... 100).contains(reported) {
            let next = Int(reported)
            if let percent, next < percent { return }
            percent = next
        }
        switch status {
        case "CreatingStagingDirectory", "TakingInstallLock", "StagingPackage":
            title = "Preparing installation…"
        case "ExtractingPackage":
            title = "Unpacking app…"
        case "InspectingPackage", "PreflightingApplication", "VerifyingApplication":
            title = "Verifying app…"
        case "InstallingApplication", "InstallingEmbeddedProfile", "InstallingApplicationIdentifiers":
            title = "Installing app…"
        case "CreatingContainer", "ContainerizingApplication":
            title = "Setting up app storage…"
        case "GeneratingApplicationMap", "RegisteringApplication":
            title = "Registering app…"
        case "PostflightingApplication", "InstallComplete", "Complete":
            title = "Finishing installation…"
        default:
            if title == "Starting installation…" { title = "Installing app…" }
        }
    }
}
