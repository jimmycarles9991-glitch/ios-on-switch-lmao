---
version: 1
slug: "apps-installer"
primary_target: "apps/installer/SwitchInstallerApp.swift"
related_targets: ["apps/installer/InstallerView.swift","apps/installer/InstallerModel.swift","apps/installer/AppIcon.icon"]
---

# Installer

Mode: Operate. A Switch owner installs an IPA from Files or a direct download link, entirely on-device.
The approved flow is import, review, confirm, install, open. The app ships on iPadOS 17 and 27.

## Direction contract

THESIS: Native source selection pushes preparation and package review onto a navigation stack.
Installation requires explicit confirmation.

OWN-WORLD: System iPad navigation, San Francisco, semantic backgrounds and labels, and standard blue controls.
The docs site's typography and plate treatment do not apply to this native app. The Composer icon uses an amber
package and a graphite installation tray.

STORY: Choose a downloaded IPA or paste its link. Review its identity and version, install it, then open it.
Progress distinguishes downloading, checking and installing. Errors explain the next useful action.

FIRST VIEWPORT: Apple's large Installer title with Choose IPA and Download from Link in a full-width plain list.
The next screen shows the package's name, version, identifier and size before Install, with a system back button.
Confirmation becomes progress and then Open.

FORM: The user-approved native workspace and review flow take precedence over the surface roll;
seed:8dd34db8ba82911b. Portrait and landscape use the available width and scroll at large text sizes.

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md,
and every shipping raster carrying its provenance.
