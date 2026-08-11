# PIpc protocol

PIpc is the socket protocol legacy webOS apps used to talk to `LunaSysMgr`. It
carries **window lifecycle and input** — never pixels; the app drew straight to
`/dev/fb0`.

Applications using this project's `libpdl` need no PIpc at all: they get their
window from Wayland through SDL2 and their identity from `appinfo.json`. This page
exists for the case where an app is still running Palm's original `libnapp`, which
insists on the handshake, and because the message IDs were expensive to recover.

`src/pdkhost.c` is a standalone host that speaks it. It is built **natively** for
the host architecture, not ARM, because it stands in for LunaSysMgr — the other
side of the emulation boundary.

## Wire format

**Socket:** `/tmp/pipcserver.<name>`. For PDK apps that is
`/tmp/pipcserver.sysmgr`, and they announce themselves as `com.palm.app.SDL`.

**Hello:** 256 bytes — `int32 pid` followed by a NUL-terminated name.

**Frame:**

```
int32  length
struct Header {
    int32  routing;
    uint16 type;
    uint16 flags;
    uint32 id;
}
payload
```

**Payload** is a Chromium `Pickle`: every field padded to a 4-byte boundary, and a
`std::string` is an `int32` length followed by the padded bytes.

**Flags:**

| | |
|---|---|
| `SYNC` | `0x4` |
| `REPLY` | `0x8` |
| `REPLY_ERROR` | `0x10` |
| `UNBLOCK` | `0x20` |

## Message IDs

> **The message IDs in `openwebos/luna-sysmgr-ipc-messages` are a different
> revision from the 3.0.5 device build.** Apps emit IDs past that table's maximum.
> Do not trust the open-source table against a device binary.

Two things went wrong before this was understood. First, the ID base was computed
assuming a single `IPC_BEGIN_MESSAGES` block, when the header has two (`View` at
`2<<12` and `ViewHost` at `3<<12`). Then, with the base fixed, the numbers still
did not match — because the published table is simply a different revision.

The values below were recovered empirically from the shipped `libnapp.so`:

* **outbound** — the literal constants passed to `PIpcMessage()`
* **inbound** — the `NGameCard::onMessageReceived` switch, keyed on `type - 0x2019`

| direction | message | id | vs the 2013 table |
|---|---|---|---|
| in  | `View_Focus`          | 8217  | differs |
| in  | `View_Resize`         | 8218  | differs |
| in  | `View_Close`          | 8224  | differs |
| in  | `View_InputEvent`     | 8225  | differs |
| in  | `View_KeyEvent`       | 8227  | differs |
| out | `PrepareAddWindow`    | 12289 | agrees |
| out | `AddWindow`           | 12291 | agrees |
| out | `RemoveWindow`        | 12292 | agrees |
| out | `SetWindowProperties` | 12296 | 2013 says 12294 |
| out | `SetKeyboardState`    | 12343 | — |
| out | `SetOrientation`      | 12348 | past the table's end |

`src/msgnames.h` holds the generated table from the 2013 open-source revision, for
comparison.

## What `pdkhost` does

1. accepts the connection and reads the hello
2. answers `PrepareAddWindow` → `SetWindowProperties` → `AddWindow` with a
   `View_Resize` followed by a `View_Focus`
3. auto-replies to any `SYNC` message so the app never blocks
4. reads a control FIFO at `/tmp/pdkhost.ctl` for manual injection:

```sh
echo 'key 32'            > /tmp/pdkhost.ctl   # space
echo 'focus'             > /tmp/pdkhost.ctl
echo 'resize 1024 768'   > /tmp/pdkhost.ctl
echo 'quit'              > /tmp/pdkhost.ctl
```

`NAPP_USEWINDOWS=1` is required to make the original `libnapp` take the
window-creation path — without it no PIpc window messages are ever sent, and the
host sits waiting.

## luna-sysmgr's own PDK path

Upstream `openwebos/luna-sysmgr` still contains the entire PDK hosting
implementation — `Src/remote/`, `Type_PDK`, the pdk jailer. The LuneOS fork
removed it: **459 files / 5.4 MB upstream against 24 files / 564 KB in the fork**,
which is now only `Src/base` and `Src/core`.

Restoring it is feasible — the `luna-sysmgr-ipc` library is still a live LuneOS
recipe, already patched for gcc-11 and glibc-2.34. This project took the other
route: make applications not need PIpc, so that nothing has to be revived in the
system manager. That is why `pdkhost` is a debugging tool here and not a shipped
component.

## Legacy luna-service2

A related trap, on the service side rather than the window side. Legacy
luna-service2 (2.0.0-136) is wire-incompatible with LuneOS's. An app carrying the
old client library stalls for about two minutes falling back to TCP.

Symlinking the old hub socket paths makes it fail fast instead:

```sh
ln -sf /var/run/luna-service2/com.palm.hub /tmp/com.palm.public_hub
ln -sf /var/run/luna-service2/com.palm.hub /tmp/com.palm.private_hub
```

The connection then fails immediately with `Broken pipe` and PDL carries on. This
project's `libpdl` sidesteps the whole problem by shelling out to `luna-send`.
