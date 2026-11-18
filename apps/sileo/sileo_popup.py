from pathlib import Path


def adapt(root: Path) -> None:
    path = root / 'Sileo/UI/TabBarController/TabBarController.swift'
    source = path.read_text()

    def replace(before: str, after: str) -> None:
        nonlocal source
        if source.count(before) != 1:
            raise ValueError('Sileo popup layout differs from the source pin')
        source = source.replace(before, after)

    replace(
        '    override func viewDidLoad() {',
        '    private var switchUsesScenePopupDock: Bool {\n'
        '        if #available(iOS 27.0, *) {\n'
        '            return UIDevice.current.userInterfaceIdiom == .pad\n'
        '                && traitCollection.horizontalSizeClass == .regular\n'
        '        }\n'
        '        return false\n'
        '    }\n\n'
        '    override func viewDidLoad() {',
    )
    replace(
        '        if UIDevice.current.userInterfaceIdiom == .pad {\n'
        '            self.popupBar.isInlineWithTabBar = true\n'
        '            self.popupBar.tabBarHeight += 1\n'
        '        }',
        '        self.popupBar.isInlineWithTabBar = UIDevice.current.userInterfaceIdiom == .pad\n'
        '            && !switchUsesScenePopupDock\n'
        '        if self.popupBar.isInlineWithTabBar {\n'
        '            self.popupBar.tabBarHeight += 1\n'
        '        }',
    )
    replace(
        '            if UIDevice.current.userInterfaceIdiom == .pad && self.view.frame.width >= 768 {',
        '            if !switchUsesScenePopupDock && UIDevice.current.userInterfaceIdiom == .pad\n'
        '                && self.view.frame.width >= 768 {',
    )
    replace(
        '    override var bottomDockingViewForPopupBar: UIView? {\n        self.tabBar\n    }',
        '    override var bottomDockingViewForPopupBar: UIView? {\n'
        '        switchUsesScenePopupDock ? nil : self.tabBar\n    }',
    )
    replace(
        '    override var defaultFrameForBottomDockingView: CGRect {\n',
        '    override var defaultFrameForBottomDockingView: CGRect {\n'
        '        if switchUsesScenePopupDock { return .zero }\n',
    )
    replace(
        '    override var insetsForBottomDockingView: UIEdgeInsets {\n',
        '    override var insetsForBottomDockingView: UIEdgeInsets {\n'
        '        if switchUsesScenePopupDock { return .zero }\n',
    )
    path.write_text(source)
