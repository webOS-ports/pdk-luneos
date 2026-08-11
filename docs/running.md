# Running applications

## The launcher

Everything a PDK app needs that differs from a normal native app is set by
`tools/pdk-run`, installed on the target as `/opt/pdk/pdk-run`:

```sh
pdk-run <app-dir> <main-binary-relative-to-app-dir> [args...]
```

It:

1. sources `<app-dir>/pdk.env` with `set -a`, so per-app overrides are exported
2. creates the `/vfs` jail symlink if the app ships a `vfs/` directory
3. `cd`s to the binary's own directory — games load assets relative to cwd
4. applies screen-size and llvmpipe-thread defaults
5. `exec`s the binary under `qemu-arm -L /opt/pdk/sysroot` with the Wayland,
   PulseAudio and GL environment set

`set -a` matters. `qemu-arm` only forwards the *real* environment, so a shell
variable that is set but not exported never reaches the guest.

## Installing applications

```sh
tools/install-games.sh <target-host> <ipk-or-directory>...
```

For each IPK it extracts the package, restores the executable bit, rewrites
`appinfo.json` and installs it where `sam` will find it.

Three things this handles that are easy to get wrong:

* **webOS IPKs set the executable bit through `package.properties`
  (`filemode.755`), not through tar metadata.** Extract one by hand and the main
  binary lands as `0644`, so the app simply never starts.
* `appinfo.json` is rewritten to `"type": "native"` with `"main": "pdk-launch"`,
  a small wrapper that calls `pdk-run`. LuneOS `sam` has no `"pdk"` or `"game"`
  handler.
* Apps go to **`/usr/palm/applications`**, not `/media/cryptofs/apps/usr/palm/applications` —
  this `sam` does not scan the latter.

`sam` needs a restart to notice newly installed applications.

> This script writes to the filesystem directly and **bypasses `appinstalld2`
> entirely**. Nothing about it validates the install service.

## Running by hand

On an x86 development workstation, against your own Wayland compositor — much
faster to iterate than the VM:

```sh
qemu-arm-static -L $SYSROOT \
    -E LD_LIBRARY_PATH=/lib:/usr/lib \
    -E XDG_RUNTIME_DIR=/run/user/1000 -E WAYLAND_DISPLAY=wayland-0 \
    -E SDL_VIDEODRIVER=wayland \
    -E PDL_DEBUG=1 -E PDK_SHIM_DEBUG=1 \
    ./thegame
```

SDL2 2.0.14 speaks `xdg_shell` as well as `wl_shell`, so GNOME and KDE both work.
The jail can live inside the sysroot rather than at `/`, needing no root, because
qemu-user tries `<sysroot>/<path>` before `<path>`:

```sh
ln -sfn /path/to/app/vfs sysroot/vfs
```

## Per-app overrides: `pdk.env`

`pdk-run` sources `<app-dir>/pdk.env`, a plain `KEY=value` file. This exists
because a few behaviours are genuinely per-engine and there is no global default
that is right for everyone.

**Resize handling.** Most titles were written for a fixed fullscreen device and
mishandle `SDL_VIDEORESIZE`, so the shim drops those events by default — Monopoly,
Tiger Woods and Bubble Bash all need that. But Haxe/NME re-sets the video mode
about eleven times a second as normal operation (782 calls in 70 s, quite happily)
and breaks when its resize events disappear. So the three Make a Scene titles
ship:

```sh
PDK_ALLOW_RESIZE=1
```

Getting this wrong costs three titles either way, which is why it is per-app and
not a global switch. Flipping the default globally once fixed Make a Scene and
silently broke Monopoly, Tiger Woods and Bubble Bash — **re-run the whole corpus
after changing any default.**

**Screen size.** Also per-title, with no single right answer. The whole corpus at
1024×768 scores 79 %; at 320×480 a *different* set passes. HD titles pick their
asset directory by resolution — Angry Birds HD asks for 864×480 and only ships
`1024x768_palmhd` — while Pre-era titles fail outright at TouchPad size; fourteen
`com.skaljac.*` games run at 320×480 and fail at 1024×768. webOS itself ran
Pre-resolution apps in a scaled compatibility card. The equivalent here is:

```sh
PDK_SCREEN_WIDTH=320
PDK_SCREEN_HEIGHT=480
```

The headline 81 % is best-of-both, which is what per-app overrides achieve.

## Environment variable reference

### Screen and input

| variable | default | effect |
|---|---|---|
| `PDK_SCREEN_WIDTH` | 1024 | reported and requested display width |
| `PDK_SCREEN_HEIGHT` | 768 | reported and requested display height |
| `PDK_SCREEN_DPI` | 132 | value returned by `PDL_GetScreenMetrics` |
| `PDK_NO_CLAMP` | unset | do not clamp oversized `SetVideoMode` requests |
| `PDK_ALLOW_RESIZE` | unset | deliver `SDL_VIDEORESIZE` instead of dropping it |
| `PDK_MODE_CACHE` | unset | reuse the surface when a mode-set is identical |
| `PDK_NO_VJOY` | unset | disable the virtual accelerometer joystick |

### Graphics

| variable | default | effect |
|---|---|---|
| `PDK_GLES_VERSION` | auto | force ES `1` or `2` instead of probing for `glOrthof` |
| `PDK_NO_GLES` | unset | do not request an ES profile — leaves desktop GL |
| `LP_NUM_THREADS` | 4 | llvmpipe rasteriser threads. **Always pin this** under emulation |
| `LIBGL_ALWAYS_SOFTWARE` | 1 | set by `pdk-run` |
| `EGL_PLATFORM` | unset | `surfaceless` skips Mesa's DRI2/X11 probing |

### Audio

| variable | default | effect |
|---|---|---|
| `PDK_ALSA_DEVICE` | — | override the ALSA device name |
| `SDL_AUDIODRIVER` | pulse | **never set this to `dummy`** — see below |

### Identity

| variable | default | effect |
|---|---|---|
| `PDL_HARDWARE` | auto | override the reported device name |
| `PDL_HARDWARE_ID` | auto | override the reported hardware ID |

### Debugging

| variable | effect |
|---|---|
| `PDK_DEBUG` | `pdk-run` shorthand: turns on all four traces below |
| `PDL_DEBUG` | trace every `PDL_*` call |
| `PDK_SHIM_DEBUG` | trace the SDL shim — mode-sets, joystick, hooks |
| `PDK_GLES_DEBUG` | log each OES entry point as it resolves |
| `PDK_CINEMA_DEBUG` | trace the video stub |
| `PDK_PRELOAD` | `LD_PRELOAD` for the guest, e.g. `/usr/lib/crashcatch.so` |
| `CRASHCATCH_QUIET` | suppress crashcatch's banner |

## Never use `SDL_AUDIODRIVER=dummy`

It was the single largest cause of "crashes" in this project, and it was
self-inflicted — I set it to keep test output quiet.

With the dummy driver `Mix_OpenAudio` fails, the engine stores a NULL where it
expects a sound object, and dereferences it later somewhere unrelated. The
faulting instruction in Make a Scene decoded to

```
ldr r2, [r1]          ; fetch a global pointer -> NULL
ldr r3, [r2, #0x18]   ; fault
```

which is the shape of an uninitialised singleton, not an ABI mismatch.

Give the apps a real audio driver. FIFA goes from `SIGFPE` to running, and Make a
Scene turns a hard crash into a clean reported error.

## Debugging a crash

The qemu gdbstub is unusably slow for titles that need a minute of emulated
startup. `build/crashcatch.so` is an `LD_PRELOAD` fault reporter instead — it
catches SIGSEGV/SIGFPE/SIGBUS/SIGILL/SIGABRT in-process at full speed and prints
the registers plus a `dladdr`-resolved backtrace:

```sh
PDK_PRELOAD=/usr/lib/crashcatch.so pdk-run /path/to/app thegame
```

**Caveat:** unwinding *through* the ARM signal frame is unreliable. Frame 0 lands
in glibc's signal machinery, so trust the register dump and the deeper frames, not
the top of the stack. Several hours went into a fault "inside glibc with `r0=0`"
that was only the signal-delivery path.

## Two harness traps

Both of these produced fake failures during corpus testing:

* **Long paths.** Several engines have fixed-size path buffers and report
  `Path name buffer overflow`. A ~110-character scratchpad path broke 27 titles,
  all the Angry Birds among them. Copy each app to something short — `/tmp/g/<id>`.
* **Core dumps.** qemu writes a **~1 GB core dump per crash** into the app
  directory. Testing 583 titles filled a 63 GB tmpfs. Set `ulimit -c 0`.

And when scoring results automatically: **`rc=137` means the process ignored
SIGTERM and was SIGKILLed** — that is a *success*, the app was still running.
Counting it as a failure undercounts badly.
