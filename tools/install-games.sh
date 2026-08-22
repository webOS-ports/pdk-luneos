#!/bin/bash
# install-games.sh - prepare legacy webOS game IPKs and install them onto a
# LuneOS target over ssh.
#
#   tools/install-games.sh <ipk-dir> [min-free-mb]
#
# Target selection: PDK_HOST / PDK_PORT, default root@localhost:5522 (the
# emulator). halium devices have openssh installed with sshd.socket enabled, but
# no usb network - so bridge a port with adb and point the script at it:
#
#   adb forward tcp:5523 tcp:22
#   PDK_PORT=5523 tools/install-games.sh <ipk-dir>
#
# PDK_TRANSPORT=adb pushes over "adb shell"/"adb push" instead, for a device with
# no sshd at all. Either way the sam restart at the end still happens - install
# the apps by hand and they sit on disk, invisible to the launcher, because sam
# only scans at startup.
#
# For each IPK (smallest first, so the most titles fit) this:
#   * unpacks it locally and finds the app directory
#   * rewrites appinfo.json to type "native" with a pdk-launch wrapper as main
#     (LuneOS's sam understands "native"; the retail packages say "game"/"pdk")
#   * restores the executable bit, which webOS sets via package.properties rather
#     than tar metadata, so a plain extract leaves the binary at 0644
#   * repacks, copies it over and unpacks into the app directory on the target
#
# It stops once the target would drop below min-free-mb (default 700), because
# the whole catalogue is far larger than the emulator image.

set -uo pipefail

IPKDIR="${1:?usage: install-games.sh <ipk-dir> [min-free-mb]}"
MINFREE="${2:-700}"

HOST=${PDK_HOST:-root@localhost}
PORT=${PDK_PORT:-5522}
SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o PubkeyAuthentication=no -o LogLevel=ERROR -o ConnectTimeout=20"
# sam scans /usr/palm/applications. /media/cryptofs/apps/... is the classic
# webOS third-party location but this build does not pick it up.
APPS=/usr/palm/applications

# Transport. halium devices (sargo and friends) come up on adb rather than ssh,
# and the emulator is the other way round. Set PDK_TRANSPORT=ssh|adb to force it;
# otherwise prefer ssh when it answers and fall back to adb when a device is
# attached. Everything below goes through remote()/push_file() so that adding a
# transport never means hand-rolling an installer again.
TRANSPORT=${PDK_TRANSPORT:-auto}
if [ "$TRANSPORT" = auto ]; then
    if ssh -n -p "$PORT" $SSHO "$HOST" true >/dev/null 2>&1; then
        TRANSPORT=ssh
    elif command -v adb >/dev/null 2>&1 && [ -n "$(adb devices | sed -n '2p')" ]; then
        TRANSPORT=adb
    else
        echo "no target: ssh $HOST:$PORT did not answer and no adb device is attached" >&2
        exit 1
    fi
fi
echo "transport: $TRANSPORT"

# -n is essential on ssh: without it ssh swallows the loop's stdin and the
# iteration stops after the first package.
remote() {
    if [ "$TRANSPORT" = adb ]; then
        adb shell "$@" < /dev/null
    else
        ssh -n -p "$PORT" $SSHO "$HOST" "$@"
    fi
}

push_file() {   # push_file <local> <remote>
    if [ "$TRANSPORT" = adb ]; then
        adb push "$1" "$2" >/dev/null 2>&1
    else
        scp -P "$PORT" $SSHO "$1" "$HOST:$2" </dev/null >/dev/null 2>&1
    fi
}

# kept so older callers/readers of this script still resolve
sshv() { remote "$@"; }

WORK=$(mktemp -d /tmp/pdkgames.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

freemb() { remote "df -Pm / | awk 'NR==2{print \$4}'" 2>/dev/null | tr -d '\r'; }

installed=0; skipped=0
echo "target has $(freemb) MB free; keeping $MINFREE MB in reserve"

# smallest first
mapfile -t IPKS < <(ls -S -r "$IPKDIR"/*.ipk 2>/dev/null)
for ipk in "${IPKS[@]}"; do
    base=$(basename "$ipk")
    free=$(freemb)
    sz=$(( $(stat -c%s "$ipk") / 1048576 ))
    # extracted is usually a bit larger than the archive
    need=$(( sz * 3 / 2 + 50 ))
    if [ "$free" -lt $(( MINFREE + need )) ]; then
        echo "SKIP  $base (${sz}MB, needs ~${need}MB, only ${free}MB free)"
        continue
    fi

    rm -rf "$WORK/x"; mkdir -p "$WORK/x"
    if ar p "$ipk" data.tar.gz > "$WORK/d.tgz" 2>/dev/null; then :; else
        tar xzOf "$ipk" ./data.tar.gz > "$WORK/d.tgz" 2>/dev/null || \
        tar xOf  "$ipk" data.tar.gz  > "$WORK/d.tgz" 2>/dev/null
    fi
    [ -s "$WORK/d.tgz" ] || { echo "FAIL  $base (no data.tar.gz)"; continue; }
    tar xzf "$WORK/d.tgz" -C "$WORK/x" 2>/dev/null

    appdir=$(find "$WORK/x" -type d -path '*/usr/palm/applications/*' -prune 2>/dev/null | head -n 1)
    [ -n "$appdir" ] || { echo "FAIL  $base (no app dir)"; continue; }
    appid=$(basename "$appdir")
    ai="$appdir/appinfo.json"
    [ -f "$ai" ] || { echo "FAIL  $appid (no appinfo.json)"; continue; }

    main=$(grep -oE '"main"[^,}]*' "$ai" | head -n 1 | sed 's/.*"main"[^"]*"//;s/"$//')
    case "$main" in *.html|"") echo "SKIP  $appid (web app)"; continue;; esac
    [ -f "$appdir/$main" ] || { echo "FAIL  $appid (main '$main' missing)"; continue; }

    chmod +x "$appdir/$main"

    # wrapper becomes the app's entry point
    cat > "$appdir/pdk-launch" <<EOF
#!/bin/sh
exec /opt/pdk/pdk-run "\$(dirname "\$0")" "$main"
EOF
    chmod +x "$appdir/pdk-launch"

    python3 - "$ai" <<'PY'
import json,sys,re
p=sys.argv[1]
raw=open(p,encoding='utf-8',errors='replace').read()
try:
    d=json.loads(raw)
except Exception:
    # a few packages ship trailing commas or comments
    d=json.loads(re.sub(r',\s*([}\]])',r'\1',re.sub(r'//[^\n]*','',raw)))
d['type']='native'
d['main']='pdk-launch'
d.setdefault('uiRevision',2)
json.dump(d,open(p,'w'),indent=4)
PY

    ( cd "$(dirname "$appdir")" && tar czf "$WORK/app.tgz" "$appid" ) || continue
    if push_file "$WORK/app.tgz" /tmp/app.tgz &&
       remote "mkdir -p $APPS && rm -rf $APPS/$appid && tar xzf /tmp/app.tgz -C $APPS && rm -f /tmp/app.tgz"; then
        echo "OK    $appid (${sz}MB)"
    else
        echo "FAIL  $appid (transfer)"
    fi
    rm -f "$WORK/app.tgz" "$WORK/d.tgz"
done

echo
echo "target now has $(freemb) MB free"
echo "restarting the app manager so it rescans..."
# Without this the apps sit on disk and never appear in the launcher - sam only
# scans at startup.
remote "systemctl restart sam 2>/dev/null || killall sam 2>/dev/null || true"
echo "done"
