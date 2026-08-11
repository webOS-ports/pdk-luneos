# Troubleshooting

Symptom → cause. Several of these cost hours; they are recorded in the order
someone new is most likely to hit them.

## The app does not start at all

| symptom | cause |
|---|---|
| `Permission denied` on the main binary | webOS IPKs set the exec bit through `package.properties` (`filemode.755`), **not** tar metadata. Extract by hand and it lands `0644`. `chmod +x` it. |
| `error while loading shared libraries: libSDL_image.so` | Missing **unversioned** `.so` symlink. Palm's device had them; packages link those names directly, including `libPDL.so` with a capital PDL. `mk-sysroot.sh` creates them. |
| `undefined symbol: curl_easy_init` | `libpdl.so` built without its transitive dependencies. It must link `libcurl`/`libssl`/`libcrypto`/`libsqlite3` with `--no-as-needed` — other Palm libraries reach those symbols *through* it. |
| `undefined symbol: glGenFramebuffersOES` | `libGLES_CM.so` shim missing. Mesa only exposes extension entry points via `eglGetProcAddress`. |
| library reported missing even though you copied it | You copied the symlink but not its target. Copy **both**. |
| `Path name buffer overflow` | The app's path is too long — several engines have fixed-size buffers. A ~110-character path broke 27 titles. Copy to `/tmp/g/<id>`. |
| app is missing but installed | `sam` needs a restart to notice new applications. And apps must be in `/usr/palm/applications` — this `sam` does not scan `/media/cryptofs/apps/...`. |

## Floating-point values are nonsense

Check the ABI of every library in the process:

```sh
readelf -h lib.so | grep Flags
# 0x5000002  soft-float ABI   ← correct
# 0x5000200  hard-float ABI   ← wrong
```

A single hard-float library in a soft-float process corrupts every float and
double crossing into it. The entire process — game, SDL, Mesa, glibc — must be
soft-float. The only boundary with the hard-float system is the Wayland socket.

## Graphics

| symptom | cause |
|---|---|
| every `eglGetDisplay` returns `EGL_BAD_PARAMETER`, no error message anywhere | Missing **dlopen** dependency. `libEGL_mesa.so.0` needs `libxcb-randr.so.0`; `swrast_dri.so` needs `libatomic.so.1` and `libz3.so.4`. libglvnd ends with zero vendors and says nothing. Run `tools/resolve-deps.py` against the *dlopened* libraries — `ldd` will not find these. |
| every shader fails to compile, but the context is valid | sdl12-compat asked for desktop GL; PDK shaders are GLSL ES with precision qualifiers (`varying highp vec3`). Should be fixed automatically by `request_gles_profile()`; check `PDK_NO_GLES` is not set. |
| `libEGL warning: egl: failed to create dri2 screen`, `radeon: Failed to get PCI ID` | Harmless. Mesa probing DRI2/X11 and hardware before falling back to llvmpipe. `EGL_PLATFORM=surfaceless` silences it. |
| extremely slow, load average pinned | llvmpipe sized itself from the core count. **Pin `LP_NUM_THREADS`** — 32 threads under emulation is *slower* than 2. |
| the app renders at the wrong size, or picks the wrong asset set | Screen size is per-title. HD titles want 1024×768; Pre-era titles fail there and want 320×480. Set `PDK_SCREEN_WIDTH`/`HEIGHT` in the app's `pdk.env`. |
| no window appears on LuneOS, works on the dev box | luna-surfacemanager has no `xdg_wm_base`; SDL2 must be 2.0.14 or older. See [architecture.md](architecture.md#the-wayland-shell-pin). |

## Crashes

| symptom | cause |
|---|---|
| NULL dereference shortly after startup, `accel is null!` | The virtual accelerometer is disabled or missing. 296 titles open SDL joystick 0 and most never check for NULL. Do not set `PDK_NO_VJOY`. |
| NULL dereference in an unrelated place, after audio init | `SDL_AUDIODRIVER=dummy`. `Mix_OpenAudio` fails, the engine stores NULL where it expects a sound object, and faults later. **Give apps a real audio driver.** This was the single largest cause of fake crashes here. |
| crash at a fixed address inside `libSDL_ttf` | Missing webOS system fonts. 12 apps hardcode `/usr/share/fonts/PreludeCondensed-Medium.ttf` and never check `TTF_OpenFont` for NULL. |
| app oscillates between two resolutions then dies | Resize feedback loop: compositor configures the surface → sdl12-compat reports a resize → the engine re-sets the video mode → repeat. The shim drops resize events by default. If you set `PDK_ALLOW_RESIZE=1`, this is why. |
| Haxe/NME title dies where it used to work | The opposite case — NME needs its resize events. Set `PDK_ALLOW_RESIZE=1` in that app's `pdk.env`. |
| `open("/vfs/data.vfs")` fails | PDK apps ran chrooted with their own directory as `/`. `ln -sfn /path/to/app/vfs /vfs`, or let `pdk-run` do it. |
| the disk fills up during testing | qemu writes a **~1 GB core dump per crash** into the app directory. `ulimit -c 0`. |

## Services

| symptom | cause |
|---|---|
| ~2 minute stall before the app continues | Legacy luna-service2 (2.0.0-136) is wire-incompatible with LuneOS's and falls back to TCP. Symlink `/tmp/com.palm.public_hub` and `/tmp/com.palm.private_hub` to `/var/run/luna-service2/com.palm.hub`; it then fails fast with `Broken pipe`. This project's `libpdl` avoids it by using `luna-send`. |
| `libnapp` app hangs with no window | `NAPP_USEWINDOWS=1` is not set, so no PIpc window messages are ever sent. |

## Reading a crash report

`crashcatch.so` prints registers and a `dladdr`-resolved backtrace:

```sh
PDK_PRELOAD=/usr/lib/crashcatch.so pdk-run /path/to/app thegame
```

**Frame 0 is not the fault.** Unwinding through the ARM signal frame is
unreliable — the top of the stack lands in glibc's signal machinery. Trust the
register dump and the deeper frames. A fault reported as "inside glibc with
`r0=0`" is almost always the delivery path, not the faulting instruction.

## Scoring automated test runs

**`rc=137` is a success.** It means the process ignored SIGTERM and was SIGKILLed —
i.e. it was still running when the timeout fired. Scoring it as a failure
undercounts badly.

## When changing a default

Re-run the **entire** corpus, not a sample. Flipping the resize-drop and
mode-cache defaults globally once fixed Make a Scene while silently breaking
Monopoly, Tiger Woods and Bubble Bash. Behaviour that differs per engine belongs
in the app's `pdk.env`, not in a global default.
