import GameController
import HWTestCore
import HWTestKit
import UIKit

struct FocusSearch27Test: HWTest {
    let id = "input.focus_search"
    let name = "List reload focus cost and controller navigation"
    let subsystem = Subsystem.input
    let interactive = true
    let timeout: TimeInterval = 90

    func run(_ context: TestContext) async throws -> Outcome {
        if await MainActor.run(body: { GCController.controllers().isEmpty }) {
            var observer: NSObjectProtocol?
            let attached: GCController? = await context.interact("Press a Joy-Con button or connect a controller") {
                waiter in
                observer = NotificationCenter.default.addObserver(
                    forName: .GCControllerDidConnect, object: nil, queue: .main
                ) { waiter.resume($0.object as? GCController) }
                if let controller = GCController.controllers().first { waiter.resume(controller) }
            }
            if let observer { NotificationCenter.default.removeObserver(observer) }
            guard attached != nil else { return .skip("no game controller connected") }
        }
        let table = await context.onStage(fullScreen: true) { stage in
            let table = FocusReloadTable(frame: stage.bounds, style: .insetGrouped)
            table.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            table.dataSource = table
            table.register(UITableViewCell.self, forCellReuseIdentifier: "row")
            stage.addSubview(table)
            return table
        }
        await Snapshots.settle()
        guard let focus = await MainActor.run(body: { UIFocusSystem.focusSystem(for: table) }) else {
            await context.clearStage()
            return .skip("no focus system with a controller connected")
        }
        let allows = await MainActor.run { table.allowsFocus }
        await MainActor.run { table.requests = 0 }
        var updateMs = 0.0, reloadMs = 0.0, requests = 0
        let reloads = 8
        for _ in 0..<reloads {
            // A nil-result search has a one-second cooldown in 24B119.
            try await Task.sleep(nanoseconds: 1_100_000_000)
            let sample = await MainActor.run { () -> (Double, Double, Int) in
                let before = table.requests
                let start = CACurrentMediaTime()
                table.reloadData()
                table.layoutIfNeeded()
                let updateStart = CACurrentMediaTime()
                focus.updateFocusIfNeeded()
                let end = CACurrentMediaTime()
                return ((end - updateStart) * 1000, (end - start) * 1000, table.requests - before)
            }
            updateMs = max(updateMs, sample.0)
            reloadMs = max(reloadMs, sample.1)
            requests += sample.2
        }
        let idleStart = await MainActor.run { table.requests }
        try await Task.sleep(nanoseconds: 2_200_000_000)
        let (idleRequests, forcedMs) = await MainActor.run { () -> (Int, Double) in
            let idle = table.requests - idleStart
            let start = CACurrentMediaTime()
            focus.requestFocusUpdate(to: table)
            focus.updateFocusIfNeeded()
            let forced = (CACurrentMediaTime() - start) * 1000
            return (idle, forced)
        }
        try await Task.sleep(nanoseconds: 1_100_000_000)
        let (allowedRequests, editingRequests, editingAllowedRequests, allowedFocus) =
            await MainActor.run { () -> (Int, Int, Int, Bool) in
                table.allowsFocus = true
                let allowed = table.reloadRequests()
                focus.requestFocusUpdate(to: table)
                focus.updateFocusIfNeeded()
                let hasFocus = (focus.focusedItem as? UIView)?.isDescendant(of: table) == true
                table.allowsFocusDuringEditing = false
                table.setEditing(true, animated: false)
                let editing = table.reloadRequests()
                table.allowsFocusDuringEditing = true
                let editingAllowed = table.reloadRequests()
                return (allowed, editing, editingAllowed, hasFocus)
            }
        await context.clearStage()
        let (list, rows) = await context.onStage { stage in
            let rows = SidebarRows()
            let layout = UICollectionViewCompositionalLayout.list(
                using: UICollectionLayoutListConfiguration(appearance: .sidebar))
            let list = UICollectionView(frame: stage.bounds, collectionViewLayout: layout)
            list.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            list.dataSource = rows
            stage.addSubview(list)
            return (list, rows)
        }
        await Snapshots.settle()
        await MainActor.run {
            list.layoutIfNeeded()
            if let system = UIFocusSystem.focusSystem(for: list) {
                system.requestFocusUpdate(to: list)
                system.updateFocusIfNeeded()
            }
        }
        var observer: NSObjectProtocol?
        let moved: Bool? = await context.interact("Move between sidebar rows with the controller D-pad", timeout: 20) {
            waiter in
            observer = NotificationCenter.default.addObserver(
                forName: UIFocusSystem.didUpdateNotification, object: nil, queue: .main
            ) { note in
                guard let update = note.userInfo?[UIFocusSystem.focusUpdateContextUserInfoKey] as? UIFocusUpdateContext,
                    !update.focusHeading.isEmpty,
                    let previous = update.previouslyFocusedView, let next = update.nextFocusedView,
                    previous !== next, previous.isDescendant(of: list), next.isDescendant(of: list)
                else { return }
                waiter.resume(true)
            }
        }
        if let observer { NotificationCenter.default.removeObserver(observer) }
        withExtendedLifetime(rows) {}
        let connected = await MainActor.run { !GCController.controllers().isEmpty }
        await context.clearStage()
        var metrics = Metrics()
        metrics.set("controllers_connected", connected)
        metrics.set("focus_system", true)
        metrics.set("allows_focus", allows)
        metrics.set("reloads", reloads)
        metrics.set("reload_requests", requests)
        metrics.set("idle_requests", idleRequests)
        metrics.set("update_ms", updateMs, digits: 1)
        metrics.set("reload_ms", reloadMs, digits: 1)
        metrics.set("forced_update_ms", forcedMs, digits: 1)
        metrics.set("allowed_requests", allowedRequests)
        metrics.set("editing_requests", editingRequests)
        metrics.set("editing_allowed_requests", editingAllowedRequests)
        metrics.set("allowed_focus", allowedFocus)
        metrics.set("sidebar_moved", moved == true)
        return .judged(
            [
                ("controller remains connected", connected),
                ("plain table retains UIKit's nonfocusable default", !allows),
                ("plain reloads request no focus updates", requests == 0),
                ("idle table requests no focus updates", idleRequests == 0),
                ("pending focus work stays under a frame", updateMs < 16.7),
                ("focus-enabled reload still requests an update", allowedRequests > 0),
                ("focus-enabled table can focus a row", allowedFocus),
                ("editing respects allowsFocusDuringEditing", editingRequests == 0 && editingAllowedRequests > 0),
                ("controller moves focus between sidebar collection rows", moved == true),
            ], metrics)
    }
}

@MainActor
private final class FocusReloadTable: UITableView, UITableViewDataSource {
    var requests = 0

    override func setNeedsFocusUpdate() {
        requests += 1
        super.setNeedsFocusUpdate()
    }

    func reloadRequests() -> Int {
        let before = requests
        reloadData()
        layoutIfNeeded()
        return requests - before
    }

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int { 60 }

    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "row", for: indexPath)
        var content = cell.defaultContentConfiguration()
        content.text = "Row \(indexPath.row + 1)"
        cell.contentConfiguration = content
        return cell
    }
}
