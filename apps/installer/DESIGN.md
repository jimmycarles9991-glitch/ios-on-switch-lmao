---
name: Installer
description: Native iPad package review and installation.
colors:
  interactive: "UIColor.systemBlue"
  icon-background-light: "rgb(94% 95% 97%)"
  icon-background-dark: "rgb(12% 13% 15%)"
typography:
  navigation-title:
    fontFamily: San Francisco
  headline:
    fontFamily: San Francisco
  subheadline:
    fontFamily: San Francisco
  body:
    fontFamily: San Francisco
  caption:
    fontFamily: San Francisco
rounded:
  package-icon: "22%"
spacing:
  metadata: "4pt"
  row-vertical: "8pt"
  progress: "12pt"
  package: "16pt"
components:
  source-action:
    typography: "{typography.body}"
  primary-action:
    typography: "{typography.headline}"
  download-link:
    typography: "{typography.body}"
  navigation:
    typography: "{typography.navigation-title}"
  package-summary:
    typography: "{typography.headline}"
  progress:
    typography: "{typography.headline}"
  technical-details:
    typography: "{typography.caption}"
---

# Design System: Installer

## Overview

Source selection opens a review screen in a system navigation stack. Installation requires explicit confirmation.

**Key Characteristics:**

- System blue on native controls.
- System text styles and SF Symbols.

## Colors

### Primary

`UIColor.systemBlue` supplies the interactive tint. Prominent buttons use the system's label styling.

### Neutral

Plain lists supply semantic backgrounds and separators.
Text uses primary and secondary foreground styles.
The icon has separate light and dark background tokens. Installed status uses `.green` with a text label and symbol.

## Typography

San Francisco follows Dynamic Type through system styles. Installer uses the system large navigation title;
review and the link sheet use inline titles.
Package names, progress titles, status labels and primary actions
use `.headline`. Versions and byte counts use `.subheadline`; technical details use `.caption`.
Body text and form controls inherit system defaults. Download byte counts add `.monospacedDigit()`.

## Layout

Lists fill the available width with system row margins and no extra top padding.
The source and review lists hide outer section separators. Dividers separate source actions and metadata rows;
the app summary has none.
Package metadata uses the metadata and package spacing tokens; progress uses the progress token.
Package and progress rows use the row-vertical padding token. The link sheet supports medium and large detents.

## Elevation & Depth

System navigation and sheet presentation supply interface depth. iPadOS 27 draws the standard Liquid Glass back button.
The app icon layers an amber package above a graphite tray. Its neutral group shadows have opacity 0.30 and 0.35.

## Shapes

System controls supply list and button shapes. Package artwork uses a scaled 64 pt base size
and a corner radius of `iconSize * 0.22`. SF Symbols supply the fallback package and status imagery.

## Components

- Source actions stay on the root screen, behind preparation and review.
- Review shows package name, version, identifier, installed size and source before explicit Install or Update
  confirmation.
- Install, Update and Open use `.borderedProminent`, `.headline` and a minimum height of 44 pt.
- Installation has one progress row. The stage and percentage come from the installation service; until a
  percentage arrives the bar is indeterminate. Download progress uses bytes received.
- Cancel appears during importing, downloading and checking. Installation progress has no cancel action.
- Back cancels preparation or discards the staged package. It is hidden during installation.
- Failures show a message and a technical-details disclosure. Installation retry appears only for
  `.failed(_, retryInstallation: true)`.
- The link sheet uses a URL text field, Cancel and a Download action disabled until the link is valid.
