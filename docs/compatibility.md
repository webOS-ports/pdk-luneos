# Compatibility

## The catalogue

`/media/herrie/HaliumDisk/ipk` holds 3901 packages. Classifying every one by its
`appinfo.json` `"type"` (cheaply — `tar --occurrence=1` stops at the first match):

| type | count |
|---|---|
| web | 3242 |
| pdk | 551 |
| game | 85 |
| native | 14 |
| scene | 6 |

**636 native titles**, 14 GB of IPKs. `"pdk"` is SDK and homebrew apps, `"game"`
is retail EA/Gameloft/Glu. Both are native and take the same path.

## Results

583 testable native titles, run headless on an x86 host under `qemu-arm`:

| | |
|---|---|
| **runs** | **478** |
| fails | 105 |
| | **81 %** (started at 71 %) |

And on the LuneOS `qemux86-64` emulator, launched through `sam` and composited by
luna-surfacemanager — 26 titles installed, **22 run**:

> Quake HD, EA Monopoly, NFS Undercover, Tiger Woods 09, Azada HD, Bubble Bash,
> Aftermath X HD, Pool 3D, Make a Scene (Farm / Jungle / Polar), Let's Golf,
> Shrek Karting (SD + HD), Earthworm Jim, Asphalt 5, Brothers in Arms 2, Avatar,
> Driver HD, NOVA 2, ActionRacing3D
>
> Failing: EA FIFA, AG HD, Glu Transformers, HAWX, Asphalt 6

## What "runs" means

The process survives 45 seconds with a live window on the compositor. It is a
**startup gate**, not a playthrough. Nothing here claims a title is completable,
or that its audio, input mapping and framerate are right. Expect rough edges past
the title screen.

The measure was chosen because startup is where the ABI, the loader, the shims and
the GL stack all get exercised, and because it can be scored automatically across
583 titles.

## Symbol and library coverage

Separately from running anything, a static audit extracted each title's main
binary and diffed its undefined SDL/PDL/GL symbols against what this stack
exports. **593 distinct symbols wanted; 560/560 apps now have complete coverage.**

That result is worth more than any individual game working: it means no title
fails merely because an entry point is absent. Whatever remains is behaviour, not
linkage.

Library coverage is near-complete. Scanning every `NEEDED` entry across all 610
native titles — main binary **and** bundled `.so` files — 10 titles reference a
soname that is not present. One is the **PowerVR SGX driver**, as dead an end as
the Adreno blob; the rest are ordinary open-source packages, itemised in
[reverse-engineering.md](reverse-engineering.md#how-much-proprietary-code-is-actually-load-bearing).

## The fixes, ranked by titles recovered

Every improvement came from clustering failures, not from debugging titles one at
a time. In rough order of impact:

### 1. The virtual accelerometer — ~hundreds of titles at risk

webOS exposed the accelerometer as SDL joystick 0 and it was always present. **296
of 583 titles call `SDL_JoystickOpen`** and 213 read its axes; most never check the
result, so with no joystick they take NULL back and segfault. EA's Sims prints
`accel is null!` on the way down.

The shim now presents one always-available three-axis device reading level. Real
joysticks take precedence, `PDK_NO_VJOY` disables it.

### 2. Missing symbols — 25+ titles

The audit found 14 absent entry points, all now implemented:

* `glWeightPointerOES`, `glMatrixIndexPointerOES`, `glCurrentPaletteMatrixOES`
  (`GL_OES_matrix_palette`, 13 titles), plus `glIsFramebufferOES`,
  `glIsRenderbufferOES` and the `glDrawTex*OES` family
* the in-app purchase API — `PDL_PurchaseItem`, `PDL_GetAvailableItems`,
  `PDL_GetItemReceiptJSON`, `PDL_GetItemCollectionJSON`, `PDL_GetParamJson`
  (12 titles)

### 3. libpdl's transitive dependencies — 7+ titles

The original `libpdl` linked `libcurl`, `libssl`, `libcrypto` and `libsqlite3`,
and other Palm libraries reached those symbols *through* it. Dropping them killed
the entire Hexage catalogue with `undefined symbol: curl_easy_init`. Restored with
`--no-as-needed`.

### 4. Unversioned `.so` symlinks — pure packaging

Palm's device carried them and plenty of packages link those names directly:
`libSDL_image.so`, `libSDL_ttf.so`, `libz.so`, `libstdc++.so`, even `libPDL.so`
with a capital PDL. Without them the loader gives up before the app runs.

### 5. Missing webOS system fonts — 12 titles, 11 fixed

Twelve apps crashed at the same address inside `libSDL_ttf` because they hardcode
`/usr/share/fonts/PreludeCondensed-Medium.ttf` and never check `TTF_OpenFont` for
NULL.

LuneOS turns out to ship **the entire Prelude family already**, via
`luna-init-fonts` — all 34 faces the legacy image had. The only fonts the legacy
image carried that LuneOS does not are the Microsoft core fonts (Arial, Courier
New, Georgia, Times New Roman, Verdana, Lucida Console) and four CJK faces, which
Palm licensed. Liberation is metric-compatible with the first three, so text laid
out for Arial still fits.

Two related dead ends, both reverted: swapping in Debian's `SDL_mixer` made NME
strictly worse (`Null Function Pointer`), and swapping `SDL_ttf` was unnecessary —
the missing *font* was the whole problem.

### 6. `PDLNet_Get_Info`

My first stub guessed the signature as `(char *buf, int *len)` and wrote through
the wrong argument, segfaulting X-Plane. It is `PDL_GetNetInfo` under another name,
taking an output struct:

```c
int PDLNet_Get_Info(const char *interfaceName, int *info)
```

### 7. `libpalmvibe.so` and `libamrnb.so.3`

`libpalmvibe.so` comes from a Veer 2.1.1 image, not the TouchPad one.
`libamrnb.so.3` is a soname **no image ships** — symlink it to the real codec.

## Two "failures" that were the harness

Worth stating separately, because they cost real time and inflated the failure
count:

* A ~110-character scratchpad path overflowed several engines' fixed-size path
  buffers. Copying each app to `/tmp/g/<id>` recovered **27 titles**, all the Angry
  Birds among them.
* Forcing 320×480 on TouchPad-only HD titles broke **16 titles**, Duke Nukem 3D
  included.

## The remaining 105

Mostly segfaults inside the games' own code, at distinct addresses, with no shared
cause left to find. Two clusters were investigated at length without result:

**The six Glu titles** (deer4 and relatives) looked like they must share a cause.
They do not. `deer4` crashes at `deer4+0x8d8d4` — a NULL dereference,
`si_addr=0` — *before* `SetVideoMode`, identically with and without the virtual
joystick, with no useful output. Ruled out: the audio driver (all four variants
segfault), and missing joystick entry points (all three it uses are overridden).

**FIFA and NFS** both fault immediately after a *successful* `SetVideoMode` — FIFA
with `SIGFPE`, NFS with `SIGSEGV`. Ruled out by experiment: resolution (identical
at 320×480, 480×320 and 1024×768), missing data files (the `/vfs` jail fixes those
errors and the crash persists), and surface pitch. The register dump puts the fault
inside glibc with `r0=0`, but that is the signal-delivery path rather than the
faulting instruction, so it is not conclusive.

Incidentally NFS is not an EA "FUSE" title at all — it is a **J2ME/MIDP** game on a
C++ MIDP runtime (`MonkeyApp`, `midp::MIDlet`, `midp::Display`), ported via iOS
(`applicationDidFinishLaunching`). Its binary keeps its symbol table, which is why
it was tractable at all.

## Case study: Make a Scene

A worked example of a diagnosis that went through three wrong theories first.

Ruled out, each by experiment:

* **Not the oversized mode.** It crashes identically when the display is
  *reported* as 1920×1080, and `SetVideoMode(1920,1080)` actually **succeeds** —
  surface returned, `pitch=7680`. So not the NULL-surface theory either.
* **Not the audio backend.** Replacing Palm's `libSDL_mixer` with Debian's makes it
  worse: NME then cannot resolve its native primitives and dies earlier with
  `Null Function Pointer`.
* **Not the surface pitch.** sdl12-compat already reports a correct pitch for GL
  surfaces (`pixels` is NULL, which is normal for `SDL_OPENGL`). A speculative
  pitch patch was written, disproved by its own diagnostic, and removed.

What actually happens: the app calls `SetVideoMode` over and over, oscillating
between 1024×768 and 1920×1080. It is a **resize feedback loop** — the compositor
configures the surface, sdl12-compat reports a resize, NME re-sets the video mode,
repeat — until the app's own state goes bad. The crash backtrace is ~24 frames
entirely inside `MyApplication` (Haxe-generated) and `nme.so`, not in any library
this project supplies. On a host compositor the same loop runs but the app exits
cleanly instead of faulting, which fits state corruption rather than a hard ABI
break.

The fix is `drop_resize()` in the SDL shim, with `PDK_ALLOW_RESIZE=1` for NME,
which needs its resize events. Make a Scene went from dozens of mode-sets to one.

## Test methodology

* Titles run headless on an x86 host under `qemu-arm`, against a real Wayland
  compositor — far faster to iterate than the emulator VM.
* Each app is copied to a short path (`/tmp/g/<id>`) before launch.
* `ulimit -c 0`.
* A title passes if the process is still alive after 45 s. `rc=137` — SIGKILL after
  ignoring SIGTERM — counts as **alive**.
* After any change to a default, the **whole** corpus is re-run. Partial re-runs
  hid three regressions once already.
