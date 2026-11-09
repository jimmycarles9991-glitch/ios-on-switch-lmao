## Don't poll

Lean towards using hardware interrupts for anything that would be a PIO poll otherwise. CPU for a kernel is a scarce
resource, we need to leave as much as possible for the applications running on it.

## Use DMA over PIO

When copying data, lean towards using DMA-like solutions instead of forcing the CPU to chaperone every byte to the right
destination. Rarely does DMA perform better than spinning on PIO so when a measurement says that its the wrong thing,
question it. Scrutinise until you are absolutely certain that it's the right course of action. A 0.1% CPU usage on a USB
driver doesn't mean PIO is the right way to do so if USB wasn't under any meaningful load at the time of capture. Apply
this same logic to other things.

## Respect Apple's assumptions

If you see a behavior wherein some persistent Apple service or application keeps using up CPU/GPU resources, don't just
say "ah well it just do be like that". Investigate. XNU/Darwin and iOS is built on a certain set of assumptions. It is
up to us to conform to them as they're likely more correct than whatever we come up with. If a widget on SpringBoard is
using a bunch of CPU, it's likely us that built something wrong instead of the widget just being like that. Always
investigate and fix such things.

## Crashes in Apple code are our bugs

When an Apple component crashes or misbehaves (a null dereference inside Metal Performance Shaders, a widget rendering
black, a daemon that spins), assume we're breaking a contract it relies on. Something our GPU driver, Metal device,
IOKit service, IOSurface, device tree or kernel should provide is missing or wrong, and the same gap will break other
components too. Find that contract, fix it at the source and check what else depends on it. Don't stub out or
special-case whichever component happened to crash first.

Patching Apple's code directly is only for code that is right for Apple's hardware and wrong for ours because our screen
or hardware differs, like SpringBoard's layout metrics at our UI scale. Even then, give the component the right input if
you can instead of patching it.

## Release builds carry no debug work

Release builds keep Apple's RELEASE options: Apple's printfs are stripped and there is no `-v` console. They keep panic
text, the saved panic record and error logs (os_log, IOLog). Every other diagnostic compiles out of release: periodic
stats, probes, self-test timing, journals, and observers on hot paths such as the per-syscall observer. Gate a
diagnostic on `DEVELOPMENT || DEBUG` where it is built, not on a runtime flag.

## Contain version-specific fixes

One source tree supports several iOS versions (17 on xnu-10063 and 27 on xnu-12377 today, more later). A fix for one
version must not change how another behaves. Put the difference where versions already differ: the per-build tables
(`kits/SwitchABI/kinds/<build>`, `switch_ui_scale_sites_<build>.h`, `ipsw/pins.bzl`), that version's own XNU tree, or a
build-time `#if`. Anything read from one IPSW, such as a UUID, a build string, an offset, a selector number or
`SetupVersion`, comes from that IPSW's pins, never from a literal that quietly applies to every version. Check every
change on both versions: both builds and both emulator checks.

## File Length & Random Tips

Don't write stupidly long files. Around 500 lines of code, you should consider splitting them. Keep helpers away from
higher level logic. A human needs to be able to read and maintain this codebase. That means you need to keep it readable
to a human. This does not conflict with code comments. Your code should be self explanatory to the largest extent
possible.

## Writing Documentation

When writing documentation (this includes even any README.md), write only what isn't obvious. When writing readmes for
individual packages, don't make them a million lines long and containing all sorts of flowery language trying to explain
every single thing about it. Keep it small, simple, bounded. If you followed the above rule, the documentation should be
the code. For things that are implicit or non-obvious (like Bazel tasks, for example), put them in the README.md files.
To make them pleasant to read, always run the `/humanizer` skill on them. Don't explain names, or folder layouts, or
anything that's a waste of space. If it's a readme, explain what this is, what it does, how to run it, why it is the way
that it is, and that's it. Do **NOT** touch `docs/` without explicit human approval. This contains documentation that
will end up on a public website and it cannot become AI slop.

Don't list what something isn't, and don't add "prior art" lists.

## Code Comments

Default to writing no new comments.

Add a comment only when it conveys information that a competent
reader cannot reasonably infer from the code itself, such as:

- Non-obvious invariants or constraints
- Why an unconventional implementation is necessary
- Hardware, protocol, or platform-specific quirks
- Subtle concurrency, ordering, or correctness requirements
- Important tradeoffs that future maintainers might otherwise undo

Never write comments that:

- Narrate what the next line or block of code does
- Restate names, types, or obvious control flow
- Divide code into sections with decorative headings
- Describe what you changed, fixed, or added
- Reference the current task or conversation
- Repeat information already expressed by the code

Prefer clear naming and structure over explanatory comments.

Preserve useful existing comments and required API documentation.
Do not add comments or docstrings to unrelated code.

Before finishing, review comments introduced in your diff.
Remove any that do not meet the criteria above.

## Naming

Never write "IOSwitch" or "ioswitch" (any case) in code, file names, logs, paths or identifiers.

- Drivers, classes and log prefixes start with `Switch`: `SwitchSD`, `SwitchCryptex`.
- C identifiers start with `switch_`; macros start with `SWITCH_`.
- Bundle IDs: `com.brunostjohn.switch.<name>`.
- Paths on the iOS filesystem use `Switch`: `/private/var/Switch/...`.
- Bazel module name and host-side tool data: `switch-ios` (`~/Library/Application Support/switch-ios`).
- SD card:
  - `switchroot/ios/`;
  - `bootloader/ini/ios.ini`, hekate entry `iOS`;
  - `bootloader/payloads/install-ios.bin`;
  - `bootloader/res/icon_ios_hue.bmp`.
- Project name `ios-on-switch-lmao`: GitHub `brunostjohn/ios-on-switch-lmao`, docs `ios-on-switch-lmao.brunostjohn.com`.
- Source extensions:
  - `.c` for C, `.cpp` for C++, `.h` for headers, `.mm` for Objective-C++, `.S` for assembly;
  - no `.inc`, `.cc`, `.cxx` or `.hpp`.
