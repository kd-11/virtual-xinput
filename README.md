# virtual-xinput

A drop-in `xinput1_3.dll` that presents a DirectInput gamepad to a game as an
XInput controller. Built for old games that only speak XInput and refuse to see
a DirectInput pad.

No installer, no driver, no service, no background process. The DLL sits in the
game's folder and does its work inside the game.

## Quick start

Run `build.ps1`, then use the configurator to install into a game:

```
dist\virtual-xinput-config.exe add "C:\Games\Some Old Game"
dist\virtual-xinput-config.exe install 0
```

Or run `dist\virtual-xinput-gui.exe` and work it out by looking at it. Both
configurators do the same job; the windowed one shows the pad moving while you
map it.

To do it by hand instead: copy `dist\x86\xinput1_3.dll` next to the game's
`.exe` — the **32-bit** build for a 32-bit game, **64-bit** for a 64-bit one.
Most old games are 32-bit. Installing through the configurator is worth
preferring because it reads the game's PE header and picks the right one; the
wrong architecture is the most common reason a dropped-in wrapper silently does
nothing.

No config file is needed. If the pad behaves, you are done.

Two things to know before you install anything: while the wrapper is in place
the game sees **only** the pads it maps, so a real Xbox controller becomes
invisible to that game; and it should never be installed into an online game
with anti-cheat. Both are covered in
[Caveats and known issues](#caveats-and-known-issues).

## The dist folder

`build.ps1` produces a clean, portable folder with no build artefacts:

```
dist\
  virtual-xinput-gui.exe       the windowed configurator
  virtual-xinput-config.exe    the same thing at a command line
  virtual-xinput.yml           reference config, fully commented
  README.md
  games.yml                    your game list (created on first use)
  x86\  xinput1_3.dll  xinput1_1.dll  xinput1_2.dll  xinput1_4.dll  xinput9_1_0.dll
  x64\  (the same, 64-bit)
```

Copy that folder anywhere — a USB stick, another machine. The game list lives
beside the executable, so it travels with it.

The `xinput1_1 / 1_2 / 1_4 / 9_1_0` files are the same code under the names
other games ask for, but they are **separate builds, not copies** — each real
Microsoft XInput DLL numbers its exports differently, and a game importing by
ordinal would call the wrong function otherwise (see *Export ordinals* below).
They are only loaded if a game actually requests that version. Build with
`-NoAliases` to get just `xinput1_3.dll`.

## Managing games

The configurator keeps a list of game folders and installs or removes the tool
from them.

```
virtual-xinput-config                    interactive menu
virtual-xinput-config games              list known games
virtual-xinput-config add <folder>       add a game folder
virtual-xinput-config install <game>     install into it
virtual-xinput-config uninstall <game>   remove it again
virtual-xinput-config list               list attached controllers
virtual-xinput-config probe [index]      controller diagnostics
virtual-xinput-config validate <file>    check a config file
```

`<game>` is either the number shown by `games` or the game's name.

Adding a folder scans it for executables and reads the PE header to decide
whether the game is 32- or 64-bit. If there are several, the menu asks which one
is the game.

The `games` menu also offers **show a game's config** and **write the current
mapping into a game's folder**, so you can run the wizard once and push the
result straight into whichever game needs it.

### Uninstalling is careful

Install records exactly which files it wrote, and uninstall removes those.

If that record is ever lost — a hand-edited or deleted `games.yml` — uninstall
falls back to checking candidates by content, and deletes a file only if it is
byte-identical to the payload. A game that ships an XInput DLL of its own is
never destroyed; it is reported and left alone.

Your `virtual-xinput.yml` is left in place on uninstall so a mapping you worked
out is not thrown away. The generated `virtual-xinput.log` is removed.

## Do you need to configure anything?

Mostly no, and for one specific reason: **axes are consistent across pads,
buttons are not.**

Nearly every PC gamepad reports its left stick on the DirectInput `x`/`y` axes,
so auto-detection gets sticks and triggers right on its own. It recognises the
three layouts that cover almost all pads:

| Axes the device reports | Interpretation | Typical hardware |
|---|---|---|
| `x y z rz` | right stick on `z`/`rz` | DualShock-style, most generic pads |
| `x y z rx ry` | right stick on `rx`/`ry`, both triggers sharing `z` | Xbox pads via the generic HID driver |
| `x y rz slider0` | right stick on `rz`/`slider0` | several Logitech pads |

Button *numbering* has no such convention. The auto-mapper assumes the common
generic order (button 0 = A, 1 = B, 2 = X, 3 = Y, …), which is right for many
pads and wrong for others. A DualShock 4 reports Square as button 0, so A/B/X/Y
come out rotated — that is what the wizard fixes, and usually the only thing
that needs changing.

## The windowed configurator

`virtual-xinput-gui.exe` is the one to reach for when a mapping is wrong and
you are not sure why. It needs nothing installed: one static executable, no
runtime, no redistributable.

- **Pad** — a virtual Xbox controller that moves as you move the real one, next
  to the mapping it came from. Click any control on the drawn pad to bind it
  ("Press A", "Push the LEFT stick RIGHT"); right-click one to clear it.
  Clicking a stick asks for both of its axes in turn. Clicking the ring around
  a stick binds the stick *click* instead.

  The deadzone sliders are live and drawn on the sticks themselves: the shaded
  disc is the deadzone, the outer ring is the saturation point, the dim dot is
  what the hardware reports and the bright one is what the game receives. When
  the two dots differ, the ring between them is why.

- **Raw input** — every axis, hat and button exactly as the device reports it,
  before any mapping. When something is wrong, this is the view that tells the
  truth about the hardware.

- **Device** — identity, what the device reports, and whether it has real
  DirectInput force feedback.

Before each prompt the controller's resting position is sampled, which is the
step that tells a trigger resting at its minimum apart from one sharing an axis
with the other trigger. Leave the pad alone while the bar fills.

Binding is not yet saved from this window — use **Save** in the console
configurator, or the Games menu there, to write `virtual-xinput.yml`. Profiles
and game management are the next piece of work; see
[docs/gui-configurator-spec.md](docs/gui-configurator-spec.md).

## The console configurator's interactive menu

- **Monitor** — every DirectInput axis, hat and button live, next to the XInput
  values they currently produce. The fastest way to see which physical control
  is which axis number.
- **Wizard** — prompts for each control in turn ("push the LEFT stick RIGHT",
  "press A"), watches what moves, and builds the mapping. `S` skips a control
  you don't have, `Q` aborts. It samples the resting position before each
  prompt, so it works out on its own whether a trigger is a full axis resting at
  its minimum or half of a shared axis resting at centre.
- **Test force feedback** — pulses each motor in turn so you can confirm rumble
  works and tell which motor is which.
- **Save** — writes `virtual-xinput.yml`.
- **Manage games** — the library described above.

## Force feedback (rumble)

`XInputSetState` is forwarded to DirectInput force feedback. Two actuators are
driven independently as the strong and weak motor; with only one, whichever
motor the game asks for more of wins.

Rumble is on by default but is **only** attempted on a device that reports force
feedback, so pads without it are unaffected. When it is used, the device must be
claimed exclusively — if that is refused because something else holds it, the
wrapper logs it, drops rumble and carries on reading input, because input
matters more.

The important caveat: **many pads only rumble through their own vendor driver,
which DirectInput cannot reach.** A DualShock 4 on the stock Windows driver
reports no force feedback at all, so there is nothing to forward. Wheels and
older gamepads with real DirectInput force feedback work. Run
`virtual-xinput-config.exe probe` to see which case a device is in.

Set `rumble: false` to keep a device in shared mode, or `rumble_gain` to soften
it.

## Configuration

`virtual-xinput.yml` goes next to the DLL (the DLL also checks beside the game
`.exe`). Everything in it is optional; anything you omit is auto-detected, so
you can override one line and leave the rest alone.

The file shipped in `dist` documents every option. The short version:

```yaml
log: false        # write virtual-xinput.log next to the dll
poll_hz: 250
rumble: true
rumble_gain: 1.0

deadzone:
  left_stick:  0.15   # radial, as a fraction of full deflection
  right_stick: 0.15
  trigger:     0.10

devices:
  - match: "*"        # substring of the product name, or "*" for any
    slot: 0           # XInput player slot 0-3
    axes:
      left_x:        x
      left_y:        -y        # '-' inverts
      right_x:       z
      right_y:       -rz
      left_trigger:  rx
      right_trigger: ry
    buttons:
      a: 0
      b: 1
      x: 2
      y: 3
    dpad: pov0
```

Axis sources are `x y z rx ry rz slider0 slider1`, optionally with a leading `-`
to invert and a trailing `+`/`-` to take only one half of the axis (for two
triggers sharing one axis). Button sources are a DirectInput button index,
`pov0:up`, `axis:z+@0.5`, or `none`.

### Deadzones

Stick deadzones are **radial**, not per-axis: they measure total deflection in
any direction, which is what actually stops a worn stick from drifting. Past the
deadzone the output is rescaled from zero, so there is no jump as the stick
crosses the boundary.

`left_stick_max` / `right_stick_max` set the saturation point — lower them if
the stick physically can't reach the corners and the game never registers a full
push.

## Troubleshooting

Set `log: true` and check `virtual-xinput.log` next to the DLL. It records the
config file used, every device found, the slot it was assigned, the mapping
chosen, and the rumble status.

| Symptom | Likely cause |
|---|---|
| Game crashes instantly on launch | Mismatched export ordinals. Rebuild; `vx_dll_host` checks them against the genuine system DLL |
| Game sees no controller | Wrong architecture — install through the configurator, which picks it from the PE header |
| Game still sees no controller | It may want a different XInput version; the alias DLLs cover that |
| A/B/X/Y rotated | PlayStation-order buttons; run the wizard |
| Character drifts on its own | Raise the `left_stick` deadzone |
| Triggers half-pressed at rest | Trigger mapped as a full axis when it shares one; run the wizard |
| Stick never reaches full | Lower `left_stick_max` |
| No rumble | The pad probably has no DirectInput force feedback — check `probe` |
| Game offers no rumble option | Capabilities report no vibration; see the caveats |
| Real Xbox pad stopped working in that game | Expected — the wrapper replaces XInput rather than extending it |
| Nothing works and the game is online | Anti-cheat. Uninstall; this tool is for old single-player games |
| Another program can't see the pad | Rumble claimed it exclusively; set `rumble: false` |

## Building

Requires MSVC and CMake.

```powershell
.\build.ps1              # build both architectures, run tests, assemble dist\
.\build.ps1 -Clean       # wipe build\ and dist\ first
.\build.ps1 -NoAliases   # only xinput1_3.dll
.\build.ps1 -NoTests     # skip the test run
```

`build.ps1` preserves `dist\games.yml` across rebuilds.

By hand, per architecture:

```sh
cmake -S . -B build/x86 -G "Visual Studio 18 2026" -A Win32
cmake --build build/x86 --config Release
cmake --install build/x86 --config Release --prefix dist
```

### Tests

```sh
build/x64/Release/vx_tests.exe                                      # logic, no pad needed
build/x64/Release/vx_dll_host.exe build/x64/Release/xinput1_3.dll   # loads the real DLL
```

`vx_tests` covers the YAML parser, config schema, deadzone maths, trigger
handling, POV decoding, the auto-mapping heuristic, PE architecture detection
and the game library round-trip. `vx_dll_host` loads the built DLL exactly as a
game does, checks every export resolves by both name and ordinal, and prints
live pad state.

## How it works

- **Export ordinals match the genuine Microsoft DLLs exactly.** This matters far
  more than it sounds: the DirectX SDK's `XInput*.lib` import libraries link
  **by ordinal, not by name**, so many games never look up a single export name.
  If the numbering is off by even one, the game calls a different function with
  a mismatched `__stdcall` signature — reading arguments that were never pushed
  and corrupting the stack — and dies with an access violation before it draws
  a frame.

  Three things make this easy to get wrong, and all three are handled:
  - The real DLLs export **`DllMain` at ordinal 1**, so the XInput functions
    start at 2, not 1.
  - Each version numbers its exports **differently**. `XInput1_3` has
    `GetState@2, SetState@3, GetCapabilities@4`; `XInput9_1_0` has
    `GetCapabilities@2, GetDSoundAudioDeviceGuids@3, GetState@4`. That is why
    every version is built from its own `.def` file rather than copied.
  - `XInput1_4` has no ordinal 6 (it dropped `GetDSoundAudioDeviceGuids`) and
    adds `GetAudioDeviceIds@10`.

  `vx_dll_host` parses the export table of the genuine DLL in `System32` and
  asserts our numbering against it, so this cannot silently regress.
- Ordinal 100 is `XInputGetStateEx`, the only entry point that reports the Guide
  button; plain `XInputGetState` masks that bit, as the real DLL does.
- It has **no XInput import of its own** — the structures are redeclared
  locally. A wrapper named `xinput1_3.dll` that linked XInput would load itself.
- `dinput8.dll` is loaded explicitly from `System32` at runtime rather than
  imported, so a `dinput8` wrapper already sitting in the game folder can't be
  picked up by accident. The DLL's only imports are `kernel32`, `user32` and
  `ole32`.
- **Nothing happens in `DllMain`.** DirectInput, threads and file I/O all run
  lazily on the first XInput call, because DllMain holds the loader lock.
- Polling runs on its own thread at `poll_hz`; exported functions only read a
  snapshot under a lock, so a slow or disconnected pad never stalls the game's
  input loop. Rumble requests are recorded and applied on that same thread,
  because every DirectInput call has to happen on the thread owning the device.
- The first XInput call blocks until the first sample has actually been
  *published*, not merely until the device is found — otherwise a game that
  checks once at startup can beat the first poll and conclude no pad exists.
  The wait is bounded at 2s: a game calling in from static initialisation still
  holds the loader lock, so the wait must be able to give up rather than
  deadlock.
- The first few samples after acquiring a device are discarded. DirectInput
  returns a zeroed state before the device has really reported, and zero is not
  neutral for a trigger that rests at its minimum — it would read as
  half-pressed.
- Devices are re-enumerated every 2s, so a pad plugged in mid-game is picked up.
- The DLL links the CRT statically, so no Visual C++ redistributable is needed
  in the game folder.

## Caveats and known issues

Read this before deciding the tool is broken. Most of what follows is a
consequence of the design rather than a bug, but all of it can look like one.

### It replaces XInput, it does not extend it

The wrapper does **not** chain-load the real `xinput1_3.dll`. While it is
installed, the game sees the DirectInput pads it maps and *nothing else* — a
genuine Xbox controller plugged into the same machine becomes invisible to that
game, because the calls that would have found it now land here instead.

That is deliberate: passing through to the real DLL would need every slot to be
arbitrated between two sources, and the whole point is to occupy slots the game
would otherwise find empty. But it means:

- Don't install into a game you also play with a real Xbox pad.
- If you want both, map the DirectInput pad to a slot the Xbox pad isn't using
  — which this build cannot do, because unmapped slots report *disconnected*,
  not *pass through*.

Uninstalling restores normal behaviour completely; nothing is left behind.

### Capabilities report no vibration

`XInputGetCapabilities` reports zero for both motor speeds even when the mapped
device does have working force feedback. A game that checks capabilities before
deciding whether to offer a rumble option may therefore hide it, even though
`XInputSetState` would have worked. Games that just call `XInputSetState`
unconditionally — most of them — are unaffected. This is a known wart, not a
deliberate choice.

### Anti-cheat

Dropping an unsigned DLL next to a game executable is exactly the shape of
thing anti-cheat systems exist to detect. Do not install this into an online
game with kernel-level anti-cheat (EAC, BattlEye, Vanguard and friends) — at
best it is refused, at worst it earns a ban. It is meant for old single-player
games.

### Games that won't load it at all

- **Microsoft Store / UWP games.** Their install folder is not writable and the
  loader won't take a side-by-side DLL.
- **Games that load XInput by absolute path** from `System32`, or through a
  launcher that pins the system copy. Rare, but nothing can be done about it
  from the game folder.
- **Games whose XInput version isn't the one you installed.** Install the alias
  DLLs (the default) so all five names are covered.
- **Architecture mismatch.** A 32-bit DLL beside a 64-bit game is silently
  ignored — no error, no log, nothing. Install through the configurator, which
  reads the PE header.

### Force feedback

- Rumble only works on a device that exposes **DirectInput** force feedback.
  Many modern pads expose it only through a vendor driver, which DirectInput
  cannot reach. A DualShock 4 on the stock Windows driver reports none at all.
  `probe` tells you which case you are in.
- Force feedback requires an **exclusive** claim on the device. While a game
  holds it, other software cannot read that pad. If the claim is refused,
  rumble is dropped and input continues.
- Effect playback has been verified against devices that report no force
  feedback (it degrades correctly) but **not yet end-to-end against real
  DirectInput FF hardware** such as a wheel. Treat the two-actuator strong/weak
  split as untested.

### Mapping

- **Button numbering is not standardised.** Auto-detection assumes the common
  generic order; a DualShock 4 reports Square as button 0, so A/B/X/Y come out
  rotated. Run the wizard. Deleting the config does not help — auto-detection
  makes the same assumption every time.
- Axes are reliable; the three layouts recognised cover nearly all pads. If a
  pad reports something else entirely, the wizard handles it.
- Only the eight standard axes (`x y z rx ry rz slider0 slider1`) and POV hat 0
  can be mapped. Devices with more hats, or with controls exposed outside
  `DIJOYSTATE2`, are out of reach.

### Runtime behaviour

- The config file is read **once**, when the DLL initialises. Editing it while
  the game runs changes nothing; restart the game.
- Hot-plug is picked up on a 2-second re-enumeration cycle, so a pad plugged in
  mid-game takes up to two seconds to appear.
- The first XInput call blocks until a real sample has been published, bounded
  at 2 seconds. A game that calls XInput from static initialisation is still
  holding the loader lock, so that wait must be allowed to time out rather than
  deadlock — in that case the game may see "no controller" on its very first
  check.
- Battery is always reported as wired and full.
- No headset audio (`XInputGetDSoundAudioDeviceGuids` returns the null device)
  and no chatpad keystrokes (`XInputGetKeystroke` returns `ERROR_EMPTY`). Both
  are what a plain wired pad reports anyway.

### Export ordinals

This is the one that crashes games rather than merely disappointing them, and
it is worth understanding because it is invisible from the outside.

Games built with the DirectX SDK link `XInput*.lib`, which imports **by ordinal
rather than by name**. If a wrapper's numbering is off by even one, the game
calls a function with a mismatched `__stdcall` signature — reading arguments
that were never pushed and unbalancing the stack — and dies with an access
violation before it draws a frame. There is no error message and no log entry;
the process simply exits with `0xC0000005`.

The numbering here is taken from the genuine Microsoft DLLs, and `vx_dll_host`
re-checks it against the copy in `System32` on every build. Two residual gaps:

- **`xinput1_1.dll` and `xinput1_2.dll` ordinals are inferred, not verified.**
  No genuine copy was available to check against, so they use the `9_1_0`
  layout. Import by name is unaffected; only a game importing *those two
  versions* by ordinal could be hurt. Delete them if such a game misbehaves —
  almost nothing needs them.
- **`XInput1_4` ordinals 108/109** are undocumented internals and are not
  exported. A missing ordinal fails to load cleanly, which is diagnosable; a
  wrong-signature stub would crash.

### Scope

- Four slots, but a device must be explicitly mapped to a slot to fill one.
  Unmapped slots report disconnected.
- Keyboard, mouse and non-joystick DirectInput devices are not sources.
- No per-game hotkeys, profiles switching at runtime, or on-screen display.
- MSVC only. The `.def`-based exports and the DirectInput libraries assume the
  Microsoft toolchain.
- The windowed configurator needs a working Direct3D 11 driver. It falls back
  to the software rasteriser where there is no usable GPU, but if even that
  fails it says so and exits; `virtual-xinput-config.exe` has no such
  requirement.
