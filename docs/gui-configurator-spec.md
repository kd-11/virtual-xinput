# Spec: GUI configurator

Status: **phases 1–4 built, 5–6 outstanding.** The window, the live pad
preview, click-to-bind and the profile store are in. The Games tab and
divergence detection are not. See *Phases* at the end for what is left.

The console configurator works and will stay. What it cannot do well is show
you a control moving while you decide what to bind it to — and that is most of
what configuring a gamepad actually is. The wizard asks "push the LEFT stick
RIGHT" and you trust it; a GUI just shows you the stick moving.

Three things are being added together because they are the same feature seen
from different angles:

1. A windowed configurator instead of a menu-driven console app.
2. A live virtual Xbox pad, so a mapping can be confirmed by looking at it.
3. Named profiles that are stored centrally, edited, and deployed into games.

## Goals

- Configuring a pad should require no understanding of DirectInput axis names.
- A mapping should be verifiable at a glance, without launching a game.
- One mapping should be reusable across many games without being re-derived.
- Every game the tool is installed into gets a config file, always.
- The result stays a portable folder: copy it to a USB stick and it works.

## Non-goals

- Replacing the console app. The CLI subcommands stay; they are scriptable and
  already tested.
- Changing the DLL. This is a front-end; `xinput1_3.dll` is untouched, and the
  config file format does not change.
- Editing a running game's mapping. The DLL reads its config once at startup;
  that stays true.
- A driver, a service, or anything resident.

## Toolkit

**Dear ImGui, Win32 + Direct3D 11 backend, vendored under `third_party/imgui`.**

The deciding constraint is that this app is fundamentally a 60 Hz view of live
hardware state. An immediate-mode UI, where every frame redraws from the
current `RawState`, is a direct expression of that; a retained-mode toolkit
would spend most of its code pushing values into widgets. Drawing a gamepad
diagram is also just a few dozen calls into ImGui's draw list, where in a
retained toolkit it is a custom control.

It also preserves what the project already has: a single self-contained
executable, statically linked, no redistributable, no installer. Vendoring
costs about 1 MB of repository and a handful of source files added to one
CMake target — no submodule, no package manager, no network at build time.

Alternatives considered:

| Option | Why not |
|---|---|
| Raw Win32 | No dependency at all, but a live pad diagram and a mapping table are hundreds of lines of `WM_PAINT` and owner-draw for no gain. |
| WPF / WinUI | Requires .NET on the target machine, or a fat self-contained publish. Ends the portable-folder property. Also splits the codebase across two languages. |
| Qt | Static linking is a licensing and build burden out of all proportion to one window. Dynamic linking means shipping DLLs. |
| Web UI in a local server | Portable in theory; in practice a browser dependency and a firewall prompt to look at a gamepad. |

D3D11 rather than D3D9 or D3D12: present on every Windows version this tool
targets, and the ImGui backend is the best-maintained of the three. The
renderer is behind ImGui's backend interface, so swapping it later is a
contained change.

## Architecture

A new executable target beside the existing one. Both link `vx_common`, which
already contains everything that matters — device enumeration, the mapping
engine, the auto-detect heuristic, the config parser and the game library.

```
vx_common  (unchanged)
   |- virtual-xinput-config.exe   console, existing, retained
   |- virtual-xinput-gui.exe      new
```

`vx_common` gains only what profiles need (see below). If the GUI turns out to
want logic the console app does not have, that logic goes in `vx_common` and
both get it — no UI-layer business rules.

The GUI opens DirectInput devices directly, in **shared, non-exclusive** mode.
It never needs the polling thread from `PadManager`; it polls on its own frame
loop, which is simpler and means closing the window releases everything. One
consequence to respect: the same "discard the first few samples after acquire"
rule applies, or a trigger reads as half-pressed on the first frame.

### dist layout after this change

```
dist/
  virtual-xinput-gui.exe       new, the thing you double-click
  virtual-xinput-config.exe    console, for scripting
  profiles/
    default.yml                always present, regenerated if deleted
    <name>.yml                 one file per profile
  games.yml                    game library, now records a profile per game
  virtual-xinput.yml           reference config, fully commented
  README.md
  x86/  x64/
```

## Screens

One window, a tab bar, no modal dialogs except confirmations.

### Pad tab — the preview

The centre of the app. Two panels side by side.

**Left: the virtual Xbox pad.** A drawn controller reflecting the XInput state
the current mapping produces right now.

- Face buttons, bumpers, Start/Back/Guide, and stick clicks light up when
  pressed.
- The D-pad lights per direction.
- Each stick is a circle with a dot at the current position. The deadzone is
  drawn as a shaded inner ring and the saturation point as an outer ring, so
  tuning a deadzone is a matter of watching where the dot stops mattering —
  which is far more direct than typing `0.15` and relaunching a game.
- Triggers are vertical bars with their 0–255 value.

**Right: the raw device.** Every axis as a labelled bar with its DirectInput
name (`x`, `-rz`, `slider0`), every POV as a compass, every button as a
numbered cell. This is the existing Monitor view, kept, because when something
is wrong it is the only view that tells you the truth about the hardware.

**Binding by clicking.** Click a control on the drawn pad and it enters capture
mode: "move the control for **A**". The next thing that moves gets bound.
Escape cancels; right-click clears a binding.

This must reuse the console wizard's inference rather than reimplement it — it
samples the resting position first, so it can tell a trigger that is a full
axis resting at its minimum from one that is half of a shared axis resting at
centre. That distinction is not something a user can be expected to explain to
the app, and getting it wrong is the "triggers half-pressed at rest" bug.

A **Reset to auto-detect** button re-runs `BuildAutoProfile` for the selected
device.

### Devices tab

Attached devices, their slot assignment, and their reported capabilities:
axis list, button count, POV count, and whether DirectInput force feedback is
present. Drag or pick to assign a device to XInput slot 0–3.

Force feedback controls live here: enable, gain, and a test that pulses each
actuator so you can tell which motor is which. Testing FF takes the device
exclusively, so the button must state that other software will lose access
while it runs, and release it immediately afterwards.

### Profiles tab

Listed profiles with the device they were built for and which games use them.
Create, duplicate, rename, delete, set as default. Deleting a profile in use
prompts and reassigns those games to the default.

A raw YAML view with the parser's error messages inline, for people who would
rather type it. Round-tripping through the editor must not destroy comments in
a hand-written file — if that proves impractical, the editor warns before it
rewrites a file it did not generate, rather than silently reformatting it.

### Games tab

The existing game library, plus a profile assignment per game.

Each row: name, folder, architecture, installed state, assigned profile. Add a
folder, install, uninstall. Installing shows what will be written before it
writes it.

Where the console app shows the architecture as a fact, the GUI should show a
warning when a folder looks like a Store/UWP install or the folder is not
writable, because both fail in ways that look like the tool not working.

## Profiles

Today a mapping is written directly into a game folder and exists nowhere else.
Work out a good mapping for a pad and you re-derive it for the next game.
Profiles fix that.

### Model

**A profile is a complete `virtual-xinput.yml`.** Not a fragment, not a new
format — the same file the DLL reads, stored centrally under a name. This
matters more than it sounds: it means a profile can be inspected, hand-edited,
copied out of the folder, or dropped into a game by hand, and it means there is
exactly one config schema in the project rather than two that must be kept in
step.

Storage is one file per profile in `profiles/`, named after a slug of the
profile name. One file per profile rather than one combined file so that a
corrupt or hand-broken profile takes only itself down.

### The default profile

`profiles/default.yml` always exists. If it is missing at startup it is
regenerated; it cannot be deleted from the UI. Its content is the fully
auto-detecting config — deadzones set, everything else left unspecified, so
the DLL's auto-detection decides. That is the configuration that works on the
widest range of hardware, and it is what a game gets when nothing else has been
chosen.

This is what makes "at least one profile is always available" true rather than
aspirational: there is no state in which installing has nothing to deploy.

### Deployment

**Install always writes a config.** `InstallGame` used to copy DLLs and nothing
else, with "write the mapping into a game folder" a separate menu action that
was easy to forget. It now copies the payload *and* writes the assigned profile
to `<game>/virtual-xinput.yml`, falling back to the default when the game has no
assignment. Games are never installed configuration-less.

`GameEntry` gained a `profile` field and `games.yml` a `profile:` key; absent
means default, so libraries written before profiles existed load unchanged.

One deliberate narrowing of the original plan: **an existing
`virtual-xinput.yml` is left alone rather than overwritten.** Someone may have
tuned it by hand, and no copy of that exists anywhere else. Until divergence
detection lands in phase 5 there is no way to ask which one the user wants, and
the safe answer when you cannot ask is not to destroy anything. Install reports
which of the two happened.

### Divergence

A config in a game folder can be edited by hand after deployment. The tool must
not pretend that cannot happen.

Install records a hash of what it wrote. When the file on disk no longer matches
it, the Games tab marks the game **modified**, and offers two explicit choices:
adopt the game's file back into the profile, or overwrite it from the profile.
It never silently picks one.

Editing a profile does not reach into game folders on its own. Games using it
are marked **out of date**, with a "redeploy" action listing exactly which
folders will be written.

### Uninstall

Unchanged in spirit: `virtual-xinput.yml` is left in place, because a mapping
someone worked out should survive removing the DLLs. The recorded file list
still drives deletion, and the byte-identical fallback still protects a game
that ships its own XInput DLL.

## Testing

The parts that can be tested without a window are the parts worth testing, and
they belong in `vx_tests` alongside the existing suite:

- Profile store: create, rename, duplicate, delete, slug collisions, round-trip
  through the YAML writer and back.
- The default profile is regenerated when absent and refuses deletion.
- `games.yml` round-trips with and without a `profile:` key; a file written by
  the current version still loads.
- Install writes a config when the game has no profile assigned.
- Divergence detection: matching hash, changed file, missing file.

The pad rendering and capture flow are inherently manual. The mapping they
display is not — it goes through the same `MapState` the DLL uses, which is
already covered.

`vx_dll_host` and the ordinal check are untouched by any of this and must keep
passing.

## Risks

- **Scope.** The pad preview is the valuable part; the profile system is the
  part that quietly grows. Build them in that order so that if the second one
  stalls, the first still shipped.
- **Two front-ends drifting.** Mitigated by putting every rule in `vx_common`,
  but it needs watching — the first time a fix lands in one and not the other,
  the console app should probably be reduced to install/uninstall only.

  Acted on during phase 3. The detection thresholds, the rest-position sampling
  and the three `*FromDetection` inferences were living in the console app's
  anonymous namespace, and the spec-string emitters were the only half of the
  config syntax not sitting beside its parser. Both moved into `vx_common`
  (`detect.h`, `config.h`) before the GUI could grow a second copy of either.
- **Comment-preserving YAML round-trip.** Likely the fiddliest piece. The
  fallback — warn before rewriting a file we did not generate — is acceptable
  and should be taken early rather than late.
- **Vendored ImGui.** Pin a release tag and record it, or updating it becomes
  archaeology.

## Phases

1. ~~**Window and monitor.**~~ **Done.** ImGui vendored at v1.92.9, both
   architectures building, device enumeration with a hot-plug rescan, the
   raw-device panel.
2. ~~**Pad preview.**~~ **Done.** The drawn controller, live, driven by the same
   `MapState` the DLL uses.
3. ~~**Binding.**~~ **Done.** Click-to-capture with a queue, resting-position
   inference shared with the console wizard, auto-detect reset, deadzone rings
   drawn over the live dots.
4. ~~**Profiles.**~~ **Done.** The store, the always-present default, the
   Profiles tab, and saving from the Pad tab. `install-writes-config` was
   pulled forward from phase 5 because without it profiles were inert.
5. **Games.** The Games tab, profile assignment from the GUI, divergence
   detection.
6. **Polish.** Store/UWP and permission warnings, FF test, README.

Phase 5 is next. Adding a game, assigning it a profile and installing are all
still console-only; the GUI can now produce and store a mapping but cannot put
one into a game folder.

## Decisions needing sign-off

- **ImGui + D3D11** as above. This is the one choice that is expensive to
  revisit later.
- **Ship both executables**, or retire the console app once the GUI covers it.
  Keeping both is proposed, but it is ongoing maintenance for a second UI.
- **Whether the GUI needs a 32-bit build.** DirectInput enumeration is not
  architecture-sensitive, so one 64-bit executable is enough on any machine
  that can run one. A 32-bit build is only needed to support 32-bit-only
  Windows, which is proposed to be dropped for the GUI while the DLL keeps
  both.
