import HWTestCore
import HWTestKit
import UIKit

/// The kernel's 24B119 cache site "UIKitCore table reload focus request only when effective focus is allowed"
/// (switch_ui_scale_sites_24B119.h): -[UITableView reloadData] asks for a focus update only when the table's
/// _allowsEffectiveFocus holds (allowsFocus, or allowsFocusDuringEditing while editing). Stock UIKitCore asks on every
/// reload. Counts the table's setNeedsFocusUpdate calls across one reloadData (`*_reload`) and the layout after it
/// (`*_layout`) with focus off and on, then while editing with focus during editing off and on. No controller is
/// needed: the site runs on every reload whether a focus system exists or not, so a wrong site shows here in Run All.
/// It runs after the other UIKit cases: a site that breaks reloadData ends HW Test here, recorded as a crash at
/// Continue run.
struct FocusReload27Test: HWTest {
    let id = "input.focus_reload"
    let name = "A table's reload asks for a focus update only when the table allows focus"
    let subsystem = Subsystem.input

    func run(_ context: TestContext) async throws -> Outcome {
        let table = await context.onStage { stage in
            let table = FocusReloadTable(frame: stage.bounds, style: .plain)
            table.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            table.dataSource = table
            table.register(UITableViewCell.self, forCellReuseIdentifier: "row")
            stage.addSubview(table)
            return table
        }
        let arms = await MainActor.run { () -> [(String, (reload: Int, layout: Int))] in
            // each change is laid out before its reload, so a request the change itself makes is not counted
            let arm = { (change: () -> Void) -> (reload: Int, layout: Int) in
                change()
                table.layoutIfNeeded()
                return table.splitRequests()
            }
            let off = arm { table.allowsFocus = false }
            let on = arm { table.allowsFocus = true }
            let editingOff = arm {
                table.allowsFocusDuringEditing = false
                table.setEditing(true, animated: false)
            }
            let editingOn = arm { table.allowsFocusDuringEditing = true }
            return [("off", off), ("on", on), ("editing_off", editingOff), ("editing_on", editingOn)]
        }
        await context.clearStage()
        var metrics = Metrics()
        var total: [String: Int] = [:]
        for (arm, counts) in arms {
            metrics.set(arm + "_reload", counts.reload)
            metrics.set(arm + "_layout", counts.layout)
            total[arm] = counts.reload + counts.layout
        }
        return .judged(
            [
                ("a reload with focus off asks for no focus update", total["off"] == 0),
                ("a reload with focus on asks for one", (total["on"] ?? 0) > 0),
                ("editing with focus during editing off asks for none", total["editing_off"] == 0),
                ("editing with focus during editing on asks for one", (total["editing_on"] ?? 0) > 0),
            ], metrics)
    }
}

@MainActor
final class FocusReloadTable: UITableView, UITableViewDataSource {
    var requests = 0

    override func setNeedsFocusUpdate() {
        requests += 1
        super.setNeedsFocusUpdate()
    }

    func reloadRequests() -> Int {
        let counts = splitRequests()
        return counts.reload + counts.layout
    }

    func splitRequests() -> (reload: Int, layout: Int) {
        let before = requests
        reloadData()
        let reloaded = requests
        layoutIfNeeded()
        return (reloaded - before, requests - reloaded)
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
