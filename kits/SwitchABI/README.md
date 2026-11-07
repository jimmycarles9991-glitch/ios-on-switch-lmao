# SwitchABI

Our drivers subclass Apple's kext classes (IOWatchdog, IOHIDDevice, ...) and call into them by address, and each IPSW
lays those classes out differently. This kit lets one driver source tree build for every IPSW kind: what differs (slot
order, instance size, field offsets, addresses) lives in tables.

- `interface/<Class>.json` is what drivers see, and it is the same for every kind: methods with their C++ declaration,
  our mangled name and Apple's PAC root name; fields by name and type; constants (offsets into structures that are not
  classes); members and C functions; `declarations`, the types its methods name that no SDK header declares (the header
  puts them before the class).
- `kinds/<kind>/<Class>.json` is one kind's layout and addresses. `found` records how each entry was read. `hints`
  holds what reading cannot name: a renamed method's slot, the string that locates a function, or a field offset or
  constant with the instruction word that shows it.

`switch_abi_class` makes `//kits/SwitchABI:<Class>`, which generates `Original<Class>.h` for the kind `--//ipsw:kind`
selects. A driver adds `abi = ["<Class>"]` to `kernel_driver` and generates its bindings with `switch_abi_bindings`.
The bindings hold every address in the table (an interface with `"bind": "imports"`: only the ones the driver's objects
import), slid by the kind's prelink slide. The build refuses an original import the table lacks, and an entry marked
`"unported"` becomes a function that panics naming the symbol. A function without a `decl` is a C++ member that the
driver's own header declares. It gets bound but not declared. A `functions_only` interface has no layout, so its header
is `struct Original<Class>` holding only the constants, plus the declared functions.

A driver class that subclasses the original names itself in `switch_abi_bindings` (`subclass`, plus `own` for the
methods it overrides). Each build then checks its compiled vtable against the kind's table: the slot count, no pure
virtual, the subclass's functions only in `own` slots, and the original's symbol in every other slot.

## Tables

```text
bazel run //kits/SwitchABI:extract                  # iOS 17 (21H461): checks the tables against the vtables
bazel run --config=ios27 //kits/SwitchABI:extract   # 24B119: rewrites kinds/24B119/*.json from the kernelcache
bazel run [--config=ios27] //kits/SwitchABI:verify  # compares instead of writing
```

The kind is whichever pinned IPSW has the kernelcache's sha256. For a fileset kernelcache (iOS 26 and later), the ipsw
CLI reads classes and inheritance, and `kernelcache.py` computes PAC discriminators and finds constructors and string
references. The same kernelcache always gives the same bytes. iOS 17's arm64 kernelcache has no PAC, so its tables were
written by hand and are only checked: the slot count, every bound slot, getMetaClass, the words of each function
hint that has a body, and each constant hint's word and value.

CI has no IPSWs, so the committed tables are what it builds from. Editing an iOS 17 table changes the iOS 17 kernel,
and `//:boot` has to stay byte-identical.
