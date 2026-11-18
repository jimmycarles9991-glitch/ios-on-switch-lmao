import Foundation

final class UITabBarItem {
    var badgeValue: String?
}
final class SplitViewController {
    let tabBarItem = UITabBarItem()
}
struct CGPoint: Equatable {
    var x: Double
    var y: Double
}
final class RefreshControl {
    struct Frame { var maxY = 44.0 }
    let frame = Frame()
    var isRefreshing = false
    var begins = 0
    var ends = 0
    func beginRefreshing() { isRefreshing = true; begins += 1 }
    func endRefreshing() { isRefreshing = false; ends += 1 }
}
final class TableView {
    struct Insets { var top = 8.0 }
    let adjustedContentInset = Insets()
    var refreshControl: RefreshControl?
    var contentOffset: CGPoint?
    func setContentOffset(_ offset: CGPoint, animated: Bool) {
        precondition(animated)
        contentOffset = offset
    }
}
struct Repo: Equatable {
    let rawURL: String
}
final class RepoManager {
    struct Call {
        let force: Bool
        let forceReload: Bool
        let isBackground: Bool
        let repos: [Repo]?
        let completion: (Bool, NSAttributedString) -> Void
    }
    static let shared = RepoManager()
    var calls = [Call]()
    func update(
        force: Bool, forceReload: Bool, isBackground: Bool, repos: [Repo]? = nil,
        completion: @escaping (Bool, NSAttributedString) -> Void
    ) {
        calls.append(Call(force: force, forceReload: forceReload, isBackground: isBackground,
                          repos: repos, completion: completion))
    }
    func completeFirst(errors: Bool = false, text: String = "") {
        let call = calls.removeFirst()
        call.completion(errors, NSAttributedString(string: text))
    }
}
final class SourcesViewController {
    var splitViewController: SplitViewController? = SplitViewController()
    let refreshControl = RefreshControl()
    var tableView: TableView? = TableView()
    var sortedRepoList = [Repo]()
    var updatingRepoList = [Repo]()
    var errors = [String]()
    init() { tableView?.refreshControl = refreshControl }
    private func addToQueue(_ repo: Repo) {
        if !updatingRepoList.contains(where: { $0.rawURL == repo.rawURL }) {
            updatingRepoList.append(repo)
        }
    }
    private func removeFromQueue(_ repo: Repo) {
        if let index = updatingRepoList.firstIndex(where: { $0.rawURL == repo.rawURL }) {
            updatingRepoList.remove(at: index)
        }
    }
    func showRefreshErrorViewController(errorOutput: NSAttributedString, completion: (() -> Void)?) {
        errors.append(errorOutput.string)
        completion?()
    }
    // SWITCH_SOURCE_METHODS
}

let first = Repo(rawURL: "https://first.example/")
let second = Repo(rawURL: "https://second.example/")
let updatesTab = UITabBarItem()
updatesTab.badgeValue = "12"

let full = SourcesViewController()
full.sortedRepoList = [first, second]
var completions = [(Bool, String)]()
full.refreshSources(forceUpdate: true, forceReload: false, isBackground: true,
                    useRefreshControl: false, useErrorScreen: true) {
    completions.append(($0, $1.string))
}
precondition(full.splitViewController?.tabBarItem.badgeValue == "…")
precondition(full.updatingRepoList.count == 2 && full.refreshControl.begins == 0)
precondition(RepoManager.shared.calls.count == 1)
let fullCall = RepoManager.shared.calls[0]
precondition(fullCall.force && !fullCall.forceReload && fullCall.isBackground && fullCall.repos == nil)
precondition(completions.isEmpty)
RepoManager.shared.completeFirst()
precondition(full.splitViewController?.tabBarItem.badgeValue == nil)
precondition(full.updatingRepoList.isEmpty && full.refreshControl.ends == 1)
precondition(completions.count == 1 && !completions[0].0 && completions[0].1.isEmpty)
precondition(updatesTab.badgeValue == "12")

let pulled = SourcesViewController()
pulled.sortedRepoList = [first]
pulled.refreshSources(forceUpdate: false, forceReload: true, isBackground: false,
                      useRefreshControl: true, useErrorScreen: true) {
    completions.append(($0, $1.string))
}
precondition(pulled.refreshControl.isRefreshing && pulled.refreshControl.begins == 1)
precondition(pulled.tableView?.contentOffset?.x == 0 && pulled.tableView?.contentOffset?.y == -52)
precondition(pulled.splitViewController?.tabBarItem.badgeValue == "…")
RepoManager.shared.completeFirst(errors: true, text: "repository failure")
precondition(!pulled.refreshControl.isRefreshing && pulled.refreshControl.ends == 1)
precondition(pulled.errors == ["repository failure"])
precondition(completions.last?.0 == true && completions.last?.1 == "repository failure")
precondition(pulled.splitViewController?.tabBarItem.badgeValue == nil)

let alreadyPulled = SourcesViewController()
alreadyPulled.refreshControl.beginRefreshing()
alreadyPulled.refreshSources(forceUpdate: false, forceReload: false, isBackground: false,
                             useRefreshControl: true, useErrorScreen: false, completion: nil)
precondition(alreadyPulled.refreshControl.begins == 1 && alreadyPulled.tableView?.contentOffset == nil)
RepoManager.shared.completeFirst(errors: true, text: "suppressed")
precondition(alreadyPulled.errors.isEmpty && alreadyPulled.refreshControl.ends == 1)

let single = SourcesViewController()
single.updatingRepoList = [first, second]
single.updateSingleRepo(first)
precondition(single.splitViewController?.tabBarItem.badgeValue == "…")
let singleCall = RepoManager.shared.calls[0]
precondition(singleCall.force && singleCall.forceReload && !singleCall.isBackground)
precondition(singleCall.repos?.map(\.rawURL) == [first.rawURL])
RepoManager.shared.completeFirst(errors: true, text: "single failure")
precondition(single.updatingRepoList.map(\.rawURL) == [second.rawURL])
precondition(single.splitViewController?.tabBarItem.badgeValue == "…" && single.refreshControl.ends == 0)
precondition(single.errors == ["single failure"])
single.updateSingleRepo(second)
RepoManager.shared.completeFirst()
precondition(single.updatingRepoList.isEmpty && single.refreshControl.ends == 1)
precondition(single.splitViewController?.tabBarItem.badgeValue == nil)

let selected = SourcesViewController()
selected.updateSpecific([first, first])
selected.updateSpecific([second])
precondition(selected.updatingRepoList.map(\.rawURL) == [first.rawURL, second.rawURL])
precondition(selected.splitViewController?.tabBarItem.badgeValue == "…")
let selectedCall = RepoManager.shared.calls[0]
precondition(!selectedCall.force && selectedCall.forceReload && !selectedCall.isBackground)
precondition(selectedCall.repos?.map(\.rawURL) == [first.rawURL, first.rawURL])
RepoManager.shared.completeFirst()
precondition(selected.updatingRepoList.map(\.rawURL) == [second.rawURL])
precondition(selected.splitViewController?.tabBarItem.badgeValue == "…" && selected.refreshControl.ends == 0)
RepoManager.shared.completeFirst(errors: true, text: "selection failure")
precondition(selected.updatingRepoList.isEmpty && selected.refreshControl.ends == 1)
precondition(selected.splitViewController?.tabBarItem.badgeValue == nil)
precondition(selected.errors == ["selection failure"])

let detached = SourcesViewController()
detached.splitViewController = nil
detached.tableView = nil
detached.sortedRepoList = [first]
detached.refreshSources(forceUpdate: false, forceReload: false, isBackground: false,
                        useRefreshControl: true, useErrorScreen: false, completion: nil)
RepoManager.shared.completeFirst()
detached.updateSingleRepo(first)
RepoManager.shared.completeFirst()
detached.updateSpecific([second])
RepoManager.shared.completeFirst()
precondition(detached.updatingRepoList.isEmpty && detached.refreshControl.ends == 3)
precondition(RepoManager.shared.calls.isEmpty && updatesTab.badgeValue == "12")
