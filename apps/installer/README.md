# Installer

Installs unencrypted IPAs on-device through MobileInstallation and the existing install policy. Open an IPA from Files,
choose one in the app, or paste a direct HTTPS link. Downloads need Installer to stay open.

```sh
bazel build --config=remote //apps/installer:SwitchInstaller17 //apps/installer:SwitchInstaller27
bazel test --config=remote //apps/installer:package_test //apps/installer:input_test
bazel build --config=remote //apps/installer:icon_previews //apps/installer:simulator27
```

Both images ship `/Applications/SwitchInstaller.app`. MobileInstallation creates its private data container during
built-in app registration. `switch-appregd` binds `com.apple.itunes.ipa` to the saved, non-placeholder record.

Device builds use pinned icon renders because `actool` output varies by worker. After editing `AppIcon.icon`,
build both device targets with `--//apps/installer:render_icons`. Run `python3 apps/installer/icon_renders.py
--icon apps/installer/AppIcon.icon --out apps/installer/artwork <17.ipa> <27.ipa>` on those outputs.
Check the resulting IPAs with `bazel run //tools/repro:check -- --remote-only //apps/installer:SwitchInstaller17
//apps/installer:SwitchInstaller27`.

Image assembly checks Installer's entitlements and records the signed files in `installer-bundle.json`.
Select a prebuilt IPA with `--//apps/installer:image_ipa=//path:seed.ipa`.

Installer uses `InstallForLaunchServices` on iOS 17 and `InstallForInstallCoordination` on iOS 27.
Imported apps retain the kernel's ordinary sandbox and capability filtering.

Downloads are limited to 8 GB and archives to 16 GB expanded. Archive inspection reads metadata and executable headers
without extracting the payload.
