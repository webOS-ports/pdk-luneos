# Reverse engineering notes

## What may and may not be published

This repository contains **only original code**. The following are deliberately
excluded from git (see `.gitignore`) and must stay excluded:

| excluded | why |
|---|---|
| `reference/` | decompiled sources and call/string maps derived from Palm/HP binaries, plus copies of those binaries |
| `sysroot/` | contains the Palm proprietary library overlay alongside Debian packages |
| `testapp/`, `*.ipk` | application packages belonging to their publishers |

Analysing binaries you possess in order to write interoperable software is one
thing; **redistributing decompiler output or the original objects is another**, and
this project does not do the second. Anyone reproducing the work supplies their own
device image via `LEGACY_ROOTFS=`.

`src/sdl_webos_shim.c` in particular is a **clean-room reimplementation**: it was
written from the exported symbol list and from observing how `libpdl` calls into
SDL, not by transcribing decompiler output.

## Why decompilation was necessary at all

**HP never released their SDL modifications.** `HP Open Source/libsdl-1.2.tgz` is
stock upstream SDL **1.2.13**: no `SDL_WebOsHook*`, no `PDL_` anything, and zero
GLES references in `src/video/fbcon/`. The shipped device binary is
`libSDL-1.2.so.0.11.2` with a GLES-capable fbcon driver. The webOS fork is simply
not in the open-source drop. (`libsdl-image`, `libsdl-mixer`, `libsdl-net` and
`libsdl-ttf` in the same drop are likewise stock.)

So for the SDL and PDL surfaces there was no source to read, and behaviour had to
come from the binaries.

## How much proprietary code is actually load-bearing

Measured, not estimated: `readelf -d` over all **610 native titles** in the
catalogue — each title's main binary plus any `.so` it bundles — tallying which
Palm-proprietary sonames appear in `NEEDED`.

| library | titles | |
|---|---|---|
| `libSDL_cinema.so` | **108 (17.7 %)** | already replaced by `src/sdl_cinema_stub.c` |
| `libnapp.so` | 1 | all four are the same title — `com.ea.app.sudoku` |
| `libPiranha.so` | 1 | " |
| `libhid.so` | 1 | " |
| `libpalmvibe.so` | 1 | " |
| `libhelpers.so`, `libhelpers-ex.so`, `libLunaKeymaps.so`, `libhal.so`, `libWebOsProxy.so`, `libdlmalloc.so`, `libaffinity.so.0`, `libgoodfork.so.0` | **0** | dropped from the overlay |

So the entire proprietary surface is **one library, already stubbed, plus a single
EA title.** That is a much better position than the size of the overlay suggests,
and it was worth measuring rather than assuming — eight libraries were being
copied out of a device image for no reason at all.

Encumbered codecs are a similarly small tail: `libavcodec.so.52` (3 titles),
`libamrnb.so.3`, `libavformat.so.52`, `libavutil.so.50` (1 each), and
`libfaac.so.0` — the one genuinely non-free item — **0 titles**.

Counting what is *absent from the sysroot today* rather than what is proprietary,
only **10 of 610 titles (1.6 %)** reference a soname that is not present, and
most of those gaps are ordinary open-source packages:

| gap | titles | |
|---|---|---|
| ffmpeg 0.8 series (`libavcodec.so.53` et al.) | 5 | LGPL; needs an old build, not a licence |
| `libpng.so.3` | 5 | the same library as `libpng12.so.0` under libpng 1.2's other soname — now symlinked |
| gupnp / gssdp | 2 | not in bookworm armel |
| `libdx.so`, `libprojectM.so` | 2 | projectM is in Debian and is now installed |
| PowerVR SGX blobs | 1 | as dead an end as the Adreno blob |
| `libcairo2`, `libfontconfig1`, `libexpat.so.0`, `libiconv.so.2` | 1 each | now installed or symlinked |

## Method

Both `libpdl.so` and `libnapp.so` in the 3.0.5 image are **unstripped**, which made
this far more tractable than it might have been.

`tools/DecompileAll.java` is a Ghidra headless script that decompiles every
function and emits a per-function call and string map alongside the C. Full
decompilation succeeded on both:

| binary | functions |
|---|---|
| `libpdl.so` | 1013 / 1013 |
| `libnapp.so` | 722 / 722 |

The call/string maps turned out to be more useful than the decompiled C itself.
Reading 444 KB of decompiler output linearly is unproductive; searching "which
functions reference this string" or "what does this function call" answers real
questions quickly.

A second reference point helped: the webOS 3.0.5 TouchPad VirtualBox image is
**x86**, so the same libraries exist compiled for a second architecture. Comparing
the two rules out architecture-specific decompilation artefacts.

## Where the ARM assembly still had to be read directly

Two findings the decompiler stated correctly but which were easy to misread.

**`CIN_Init` polarity.** The relevant sequence is

```
movne r2, #0
moveq r2, #1
```

which is a logical NOT — so **`CIN_Init()` returns non-zero for success.** My first
stub returned 0 and faithfully reproduced the exact bug it was written to prevent
(NFS Undercover's silent `initRuntimePalm` bail-out). See
[architecture.md](architecture.md#libsdl_cinemaso--srcsdl_cinema_stubc).

**PIpc message IDs.** Recovered from literal constants passed to `PIpcMessage()`
for outbound messages, and from the `NGameCard::onMessageReceived` switch — keyed
on `type - 0x2019` — for inbound. Full table in [pipc.md](pipc.md#message-ids).

## Signatures recovered by being wrong first

`PDLNet_Get_Info` is worth recording because the failure mode was instructive. The
name suggests a getter with a buffer, so the first stub guessed:

```c
int PDLNet_Get_Info(char *buf, int *len);      /* wrong */
```

and wrote through the second argument. X-Plane segfaulted. It is actually
`PDL_GetNetInfo` under a second name, taking an interface name and an **output
struct**:

```c
int PDLNet_Get_Info(const char *interfaceName, int *info);
```

with `-0x2a48` for a NULL name and `-0x2a30` for an empty one — error constants
that are themselves only visible in the disassembly.

The general lesson: a stub with a guessed signature is more dangerous than a
missing symbol. A missing symbol fails at load with a clear message; a wrong
signature corrupts memory at a distance.

## Static auditing

Most of the compatibility work came from static analysis across the whole
catalogue rather than from decompiling anything. For each of 583 main binaries:

* extract undefined symbols, diff against what this stack exports → 593 distinct
  symbols wanted, 14 missing, all now implemented
* extract every `NEEDED` entry, check each resolves in the sysroot → 10 of 610
  titles reference something absent, itemised above. An earlier pass over main
  binaries only reported this as "complete except one PowerVR title"; including
  each package's bundled `.so` files is what surfaced the other nine
* count callers of specific APIs, which is how the accelerometer fix was
  prioritised: 296 titles call `SDL_JoystickOpen`, 213 read its axes

Counting callers before implementing anything is the highest-value habit in this
project. It turns "which of these hundred bugs matters" into an ordered list.
