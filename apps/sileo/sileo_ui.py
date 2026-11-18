import re
from pathlib import Path

from apps.sileo import sileo_popup


def adapt(root: Path) -> None:
    sources = root / 'Sileo/UI/SourcesViewController/SourcesViewController.swift'
    source = sources.read_text()
    if source.count('value(forKey: "_badge")') != 4:
        raise ValueError('Sileo source refresh UI differs from the source pin')

    def prefix(signature: str, boundary: str, replacement: str) -> None:
        nonlocal source
        if source.count(signature) != 1:
            raise ValueError('Sileo source refresh method differs from the source pin')
        start = source.index(signature) + len(signature)
        end = source.index(boundary, start)
        source = source[:start] + replacement + source[end:]

    prefix(
        '    private func killIndicator() {',
        '    func refreshSources(forceUpdate: Bool, forceReload: Bool, isBackground:',
        '\n        self.refreshControl.endRefreshing()\n'
        '        self.splitViewController?.tabBarItem.badgeValue = nil\n    }\n\n',
    )
    prefix(
        '    func refreshSources(forceUpdate: Bool, forceReload: Bool, isBackground: Bool, '
        'useRefreshControl: Bool, useErrorScreen: Bool, completion: ((Bool, NSAttributedString) -> Void)?) {',
        '            if useRefreshControl {',
        '\n        self.splitViewController?.tabBarItem.badgeValue = "…"\n        if updatingRepoList.isEmpty {\n',
    )
    prefix(
        '    func updateSingleRepo(_ repo: Repo) {',
        '        RepoManager.shared.update(',
        '\n        self.splitViewController?.tabBarItem.badgeValue = "…"\n\n',
    )
    prefix(
        '    func updateSpecific(_ repos: [Repo]) {',
        '        for repo in repos {',
        '\n        self.splitViewController?.tabBarItem.badgeValue = "…"\n\n',
    )
    if 'value(forKey: "_badge")' in source or 'item?.view()' in source:
        raise ValueError('Sileo source refresh still depends on private badge views')
    sources.write_text(source)

    def replace(relative: str, before: str, after: str, count: int = 1) -> None:
        path = root / 'Sileo' / relative
        text = path.read_text()
        if text.count(before) != count:
            raise ValueError(f'{path.name}: UIKit use differs from the source pin')
        path.write_text(text.replace(before, after))

    replace(
        'AppDelegate.swift',
        '        tabBarController.tabBar._blurEnabled = true\n        tabBarController.tabBar.tag = WHITE_BLUR_TAG',
        '        tabBarController.tabBar.isTranslucent = true',
    )
    for relative, expression in (
        ('AppDelegate.swift', 'controller.tabBarItem'),
        ('UI/FeaturedViewController/FeaturedViewController.swift', 'self.navigationController?.tabBarItem'),
    ):
        replace(
            relative,
            expression + '._setInternalTitle(String(localizationKey: ',
            expression + '.title = String(localizationKey: ',
        )
        localization = 'Search_Page' if relative == 'AppDelegate.swift' else 'Featured_Page'
        replace(
            relative,
            f'.title = String(localizationKey: "{localization}"))',
            f'.title = String(localizationKey: "{localization}")',
        )

    search = 'UI/PackageListViewController/PackageListViewController.swift'
    replace(search, '.searchBar.value(forKey: "searchField") as? UITextField', '.searchBar.searchTextField', 2)
    replace(search, '.searchBar.value(forKey: "_searchField") as? UITextField', '.searchBar.searchTextField')
    for relative in ('UI/Utils/PackageIconView.swift', 'UI/SettingsViewController/Root/SettingsIconHeaderView.swift'):
        replace(relative, '.layer.setValue(true, forKey: "continuousCorners")', '.layer.cornerCurve = .continuous')
    replace(
        'Sileo-Bridging-Header.h',
        '@interface UITabBar (Private)\n'
        '@property (assign, setter=_setBlurEnabled:, nonatomic) BOOL _blurEnabled;\n@end\n\n',
        '',
    )
    replace(
        'Sileo-Bridging-Header.h',
        '@interface UITabBarItem (Private)\n- (UIView *)view;\n-(void)_setInternalTitle:(id)arg1 ;\n@end\n\n',
        '',
    )
    adapt_bars(root)
    adapt_picker(root)
    adapt_geometry(root)
    sileo_popup.adapt(root)


def adapt_bars(root: Path) -> None:
    for relative, shadows, opacity in (
        ('UI/FeaturedViewController/FeaturedViewController.swift', 2, 0),
        ('UI/NewsViewController/NewsViewController.swift', 2, 0),
        ('UI/SourcesViewController/SourcesViewController.swift', 3, 0),
        ('UI/PackageListViewController/PackageListViewController.swift', 2, 0),
        ('UI/PackageListViewController/ListHeader/PackageListHeader.swift', 1, 0),
        (
            (
                'UI/PackageViewController/DepictionView/DepictionScreenshotsView/DepictionScreenshotsViewController/'
                'DepictionScreenshotsViewController.swift'
            ),
            1,
            0,
        ),
        ('UI/PackageViewController/PackageViewController.swift', 0, 6),
        ('UI/NativePackageViewController/NativePackageViewController.swift', 0, 6),
        ('UI/SourcesViewController/SourcesSplitViewController.swift', 0, 1),
    ):
        path = root / 'Sileo' / relative
        text = path.read_text()
        if text.count('._hidesShadow') != shadows or text.count('._backgroundOpacity') != opacity:
            raise ValueError(f'{path.name}: bar styling differs from the source pin')
        text = re.sub(r'\._hidesShadow = (true|false)', r'.switchSetShadowHidden(\1)', text)
        lines = []
        for line in text.splitlines(keepends=True):
            if '._backgroundOpacity = 0' in line:
                lines.append('        self.switchConfigureScrollEdge(for: scrollView)\n')
            elif '._backgroundOpacity' not in line and 'private var currentNavBarOpacity' not in line:
                lines.append(line)
        path.write_text(''.join(lines))
    white_blur = root / 'Sileo/UI/WhiteBlur'
    original = (white_blur / 'WhiteBlur.m').read_text()
    if 'hook(_UIVisualEffectSubview)' not in original:
        raise ValueError('Sileo blur styling differs from the source pin')
    for extension in ('h', 'm'):
        content = Path(__file__).with_name(f'SwitchBarAppearance.{extension}').read_text()
        path = white_blur / f'WhiteBlur.{extension}'
        original = path.read_text()
        path.write_text(original[: original.index('#')] + content)
    bridge = root / 'Sileo/Sileo-Bridging-Header.h'
    declaration = (
        '@interface UINavigationBar (Private)\n'
        '@property (assign, setter=_setBackgroundOpacity:, nonatomic) CGFloat _backgroundOpacity '
        'API_AVAILABLE(ios(11.0));\n'
        '@end\n\n'
    )
    text = bridge.read_text()
    if text.count(declaration) != 1:
        raise ValueError('Sileo navigation styling declaration differs from the source pin')
    bridge.write_text(text.replace(declaration, ''))


def adapt_geometry(root: Path) -> None:
    tab = root / 'Sileo/UI/TabBarController/TabBar.swift'
    text = tab.read_text()
    signature = '    override var traitCollection: UITraitCollection {\n'
    if text.count(signature) != 1 or text.count('UIApplication.shared.statusBarOrientation') != 1:
        raise ValueError('Sileo tab layout differs from the source pin')
    text = text.replace(signature, signature + '        if #available(iOS 27.0, *) { return super.traitCollection }\n')
    text = text.replace(
        'UIApplication.shared.statusBarOrientation.isLandscape',
        '(window?.windowScene?.interfaceOrientation.isLandscape == true)',
    )
    tab.write_text(text)
    for relative, header in (
        ('UI/PackageViewController/PackageViewController.swift', 'depictionHeaderImageViewHeight'),
        ('UI/NativePackageViewController/NativePackageViewController.swift', 'headerImageViewHeightAnchor'),
    ):
        path = root / 'Sileo' / relative
        text = path.read_text()
        if text.count('UIApplication.shared.statusBarFrame.height') != 3:
            raise ValueError(f'{path.name}: package geometry differs from the source pin')
        pattern = (
            r'        let container = CGRect\(origin: CGPoint\(x: scrollView.contentOffset.x,\n\s*'
            r'y: scrollView.contentOffset.y \+ 106 - UIApplication.shared.statusBarFrame.height\),\n\s*'
            r'size: scrollView.frame.size\)'
        )
        text, count = re.subn(
            pattern,
            '        let container = scrollView.convert(view.safeAreaLayoutGuide.layoutFrame, from: view)',
            text,
        )
        if count != 1:
            raise ValueError(f'{path.name}: package viewport differs from the source pin')
        signature = '    func scrollViewDidEndDragging(_ scrollView: UIScrollView, willDecelerate decelerate: Bool) {'
        start = text.index(signature)
        end = text.index('\n    }', start) + len('\n    }')
        text = (
            text[:start] + signature + '\n'
            '        let expanded = -scrollView.adjustedContentInset.top\n'
            f'        let collapsed = max(expanded, {header}.constant - view.safeAreaInsets.top)\n'
            '        let offset = scrollView.contentOffset.y\n'
            '        guard offset > expanded && offset < collapsed else { return }\n'
            '        let target = offset < (expanded + collapsed) / 2 ? expanded : collapsed\n'
            '        scrollView.setContentOffset(CGPoint(x: scrollView.contentOffset.x, y: target), animated: true)\n'
            '    }' + text[end:]
        )
        if relative.startswith('UI/PackageViewController/'):
            text = text.replace(
                'bottom: self.tabBarController?.tabBar.bounds.height ?? 0', 'bottom: view.safeAreaInsets.bottom'
            )
            signature = '    override func viewDidLayoutSubviews() {\n        super.viewDidLayoutSubviews()\n'
            if text.count(signature) != 1:
                raise ValueError('Sileo package bottom inset differs from the source pin')
            text = text.replace(
                signature, signature + '        scrollView.contentInset.bottom = view.safeAreaInsets.bottom\n'
            )
        path.write_text(text)


def adapt_picker(root: Path) -> None:
    controller = root / 'Sileo/UI/SettingsViewController/Root/SettingsViewController.swift'
    text = controller.read_text()
    before = '                cell.pickerView.textColor = .sileoLabel\n'
    if text.count(before) != 1:
        raise ValueError('Sileo theme picker styling differs from the source pin')
    controller.write_text(text.replace(before, ''))
    picker = root / 'Sileo/UI/SettingsViewController/Themes/ThemePickerView.swift'
    text = picker.read_text()
    before = (
        '    func pickerView(_ pickerView: UIPickerView, titleForRow row: Int, '
        'forComponent component: Int) -> String? {\n        values[row]\n    }'
    )
    after = (
        '    func pickerView(_ pickerView: UIPickerView, attributedTitleForRow row: Int, '
        'forComponent component: Int) -> NSAttributedString? {\n'
        '        NSAttributedString(string: values[row], attributes: [.foregroundColor: UIColor.sileoLabel])\n    }'
    )
    if text.count(before) != 1:
        raise ValueError('Sileo theme picker delegate differs from the source pin')
    picker.write_text(text.replace(before, after))
    bridge = root / 'Sileo/Sileo-Bridging-Header.h'
    declaration = (
        '@interface UIPickerView (Private)\n'
        '@property (setter=_setTextColor:,getter=_textColor,nonatomic,retain) UIColor* textColor;\n@end\n\n'
    )
    text = bridge.read_text()
    if text.count(declaration) != 1:
        raise ValueError('Sileo theme picker declaration differs from the source pin')
    bridge.write_text(text.replace(declaration, ''))
