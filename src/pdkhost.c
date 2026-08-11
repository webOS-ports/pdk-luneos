// pdkhost - a minimal PIpc host for legacy webOS PDK/SDL apps on LuneOS.
//
// Speaks enough of the luna-sysmgr PIpc protocol to let an unmodified 2011 PDK
// app complete window setup and receive input. Pixels are NOT carried here -
// legacy SDL renders straight to /dev/fb0 (its only video backend is FBCON).
//
// Wire format (openwebos/luna-sysmgr-ipc):
//   hello   : 256 bytes = int32 pid + NUL-terminated client name
//   frame   : int32 length, then <length> bytes
//   message : Header{ int32 routing; uint16 type; uint16 flags; uint32 id; } + payload
//   payload : Chromium Pickle - every field padded up to a 4-byte boundary,
//             std::string = int32 length + bytes (padded)
//   type    : (ViewMsgStart=2 << 12) + declaration index, see msgnames.h
//   flags   : SYNC=0x4 REPLY=0x8 REPLY_ERROR=0x10 UNBLOCK=0x20
//
// Input injection: write a line to the control FIFO (default /tmp/pdkhost.ctl)
//   key <keycode> [modifiers]   - press+release a Qt key on the focused window
//   touch <x> <y>               - press+release at a screen coordinate
//   focus / unfocus
//   quit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "msgnames.h"

#define SYNC_BIT        0x0004
#define REPLY_BIT       0x0008
#define REPLY_ERROR_BIT 0x0010

// Message ids for the webOS 3.0.5 device build.
//
// These do NOT all match openwebos/luna-sysmgr-ipc-messages (2013) - that table
// is a later revision. The values below were recovered empirically from the
// 3.0.5 libnapp.so: outbound ids are the literal constants passed to
// PIpcMessage() in each sender, inbound ids come from the
// NGameCard::onMessageReceived switch, which is keyed on (type - 0x2019).
// Where the two tables agree it is noted; where they differ, trust these.

// --- outbound, app -> host (ViewHost block, base 0x3000 = 12288) ---
#define VIEWHOST_PREPARE_ADD_WINDOW           12289  // 0x3001, agrees with 2013
#define VIEWHOST_ADD_WINDOW                   12291  // 0x3003, agrees with 2013
#define VIEWHOST_REMOVE_WINDOW                12292  // 0x3004, agrees with 2013
#define VIEWHOST_SET_WINDOW_PROPERTIES        12296  // 0x3008, 2013 says 12294
#define VIEWHOST_SET_KEYBOARD_STATE           12343  // 0x3037
#define VIEWHOST_SET_ORIENTATION              12348  // 0x303c
#define VIEWHOST_BUFFER_LOCK                  12316  // 0x301c
#define VIEWHOST_BUFFER_UNLOCK                12314  // 0x301a

// --- inbound, host -> app (View block); switch base is 0x2019 = 8217 ---
#define VIEW_FOCUS                            8217   // case 0,  Tuple1<bool>
#define VIEW_RESIZE                           8218   // case 1,  int,int,bool
#define VIEW_CLOSE                            8224   // case 7,  Tuple1<bool>
#define VIEW_INPUT_EVENT                      8225   // case 8,  SysMgrEventWrapper
#define VIEW_KEY_EVENT                        8227   // case 10, int16,int32,int32,string
#define VIEW_FULLSCREEN_ENABLED               8229   // case 12
#define VIEW_FULLSCREEN_DISABLED              8230   // case 13, sync
#define VIEW_PAUSE                            8234   // case 17
#define VIEW_RESUME                           8235   // case 18

struct Header {
    int32_t  routing;
    uint16_t type;
    uint16_t flags;
    uint32_t id;
};

// ---------------------------------------------------------------- pickle out

typedef struct { unsigned char *b; size_t len, cap; } Buf;

static void buf_need(Buf *p, size_t extra)
{
    if (p->len + extra <= p->cap) return;
    size_t cap = p->cap ? p->cap * 2 : 256;
    while (cap < p->len + extra) cap *= 2;
    p->b = realloc(p->b, cap);
    p->cap = cap;
}

static void buf_pad4(Buf *p)
{
    while (p->len % 4) { buf_need(p, 1); p->b[p->len++] = 0; }
}

static void w_i32(Buf *p, int32_t v)
{
    buf_need(p, 4); memcpy(p->b + p->len, &v, 4); p->len += 4;
}

static void w_i16(Buf *p, uint16_t v)   // padded to 4, like Pickle
{
    buf_need(p, 4); memset(p->b + p->len, 0, 4);
    memcpy(p->b + p->len, &v, 2); p->len += 4;
}

static void w_str(Buf *p, const char *s)
{
    size_t n = strlen(s);
    w_i32(p, (int32_t)n);
    buf_need(p, n); memcpy(p->b + p->len, s, n); p->len += n;
    buf_pad4(p);
}

// ----------------------------------------------------------------- pickle in

typedef struct { const unsigned char *b; size_t len, pos; } Rdr;

static int r_i32(Rdr *r, int32_t *out)
{
    if (r->pos + 4 > r->len) return 0;
    memcpy(out, r->b + r->pos, 4); r->pos += 4;
    return 1;
}

// ------------------------------------------------------------------- sending

static int g_client = -1;

static int send_all(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char *)buf + sent, len - sent, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        sent += (size_t)n;
    }
    return 0;
}

static void send_msg(int32_t routing, uint16_t type, uint16_t flags,
                     uint32_t id, const Buf *payload)
{
    if (g_client < 0) return;

    struct Header h = { routing, type, flags, id };
    size_t plen = payload ? payload->len : 0;
    int32_t total = (int32_t)(sizeof(h) + plen);

    unsigned char *frame = malloc(sizeof(int32_t) + (size_t)total);
    memcpy(frame, &total, sizeof(int32_t));
    memcpy(frame + sizeof(int32_t), &h, sizeof(h));
    if (plen) memcpy(frame + sizeof(int32_t) + sizeof(h), payload->b, plen);

    if (send_all(g_client, frame, sizeof(int32_t) + (size_t)total) < 0)
        perror("[host] send");
    free(frame);
}

// Names verified against the 3.0.5 libnapp.so; these win over msgnames.h,
// which is generated from the later 2013 open-source headers.
static const struct { int id; const char *name; } kDeviceMsgs[] = {
    { VIEWHOST_PREPARE_ADD_WINDOW,    "ViewHost_PrepareAddWindow"    },
    { VIEWHOST_ADD_WINDOW,            "ViewHost_AddWindow"           },
    { VIEWHOST_REMOVE_WINDOW,         "ViewHost_RemoveWindow"        },
    { VIEWHOST_SET_WINDOW_PROPERTIES, "ViewHost_SetWindowProperties" },
    { VIEWHOST_SET_KEYBOARD_STATE,    "ViewHost_SetKeyboardState"    },
    { VIEWHOST_SET_ORIENTATION,       "ViewHost_SetOrientation"      },
    { VIEWHOST_BUFFER_LOCK,           "ViewHost_BufferLock"          },
    { VIEWHOST_BUFFER_UNLOCK,         "ViewHost_BufferUnlock"        },
    { 12288,                          "ViewHost_Generic(0x3000)"     },
};

static const char *msg_name(int id, int *is_sync)
{
    if (is_sync) *is_sync = 0;

    for (size_t i = 0; i < sizeof(kDeviceMsgs) / sizeof(kDeviceMsgs[0]); i++)
        if (kDeviceMsgs[i].id == id) return kDeviceMsgs[i].name;

    // fall back to the 2013 table, flagged since ids may be shifted
    for (int i = 0; i < kNumMsgs; i++)
        if (kMsgs[i].id == id) {
            if (is_sync) *is_sync = kMsgs[i].sync;
            return kMsgs[i].name;
        }
    return "?";
}

// ------------------------------------------------------------------- actions

static int32_t g_window = 0;      // routing id of the app's window
static int     g_w = 0, g_h = 0;  // last size the app asked for

static void send_focus(int on)
{
    Buf p = {0};
    w_i32(&p, on ? 1 : 0);           // bool is written as an aligned int
    send_msg(g_window, VIEW_FOCUS, 0, 0, &p);
    free(p.b);
    printf("[host] -> View_Focus(%s) routing=%d\n", on ? "true" : "false", g_window);
    fflush(stdout);
}

static void send_resize(int w, int h)
{
    Buf p = {0};
    w_i32(&p, w); w_i32(&p, h); w_i32(&p, 0);   // width, height, force
    send_msg(g_window, VIEW_RESIZE, 0, 0, &p);
    free(p.b);
    printf("[host] -> View_Resize(%d,%d) routing=%d\n", w, h, g_window);
    fflush(stdout);
}

// SysMgrKeyEvent: int16 type, int32 key, int32 modifiers, string text
static void send_key(int qt_type, int key, unsigned mods, const char *text)
{
    Buf p = {0};
    w_i16(&p, (uint16_t)qt_type);
    w_i32(&p, key);
    w_i32(&p, (int32_t)mods);
    w_str(&p, text ? text : "");
    send_msg(g_window, VIEW_KEY_EVENT, 0, 0, &p);
    free(p.b);
    printf("[host] -> View_KeyEvent(type=%d key=%d)\n", qt_type, key);
    fflush(stdout);
}

static void handle_control_line(char *line)
{
    char cmd[32]; int a = 0, b = 0;
    int n = sscanf(line, "%31s %d %d", cmd, &a, &b);
    if (n < 1) return;

    if (!strcmp(cmd, "key") && n >= 2) {
        send_key(6, a, (unsigned)(n >= 3 ? b : 0), "");   // QEvent::KeyPress
        send_key(7, a, (unsigned)(n >= 3 ? b : 0), "");   // QEvent::KeyRelease
    } else if (!strcmp(cmd, "focus")) {
        send_focus(1);
    } else if (!strcmp(cmd, "unfocus")) {
        send_focus(0);
    } else if (!strcmp(cmd, "resize") && n >= 3) {
        send_resize(a, b);
    } else if (!strcmp(cmd, "quit")) {
        printf("[host] quit requested\n");
        exit(0);
    } else {
        printf("[host] unknown control command: %s\n", cmd);
    }
    fflush(stdout);
}

// ------------------------------------------------------------------ dispatch

static void on_message(const struct Header *h, const unsigned char *pay, size_t paylen)
{
    int is_sync = 0;
    const char *name = msg_name(h->type, &is_sync);

    printf("[host] <- %-38s routing=%-3d type=%d flags=0x%04x paylen=%zu\n",
           name, h->routing, h->type, h->flags, paylen);

    Rdr r = { pay, paylen, 0 };

    switch (h->type) {
    case VIEWHOST_PREPARE_ADD_WINDOW: {
        int32_t key = 0, type = 0, w = 0, hh = 0;
        r_i32(&r, &key); r_i32(&r, &type); r_i32(&r, &w); r_i32(&r, &hh);
        g_window = key; g_w = w; g_h = hh;
        printf("[host]    window key=%d type=%d size=%dx%d\n", key, type, w, hh);
        break;
    }
    case VIEWHOST_ADD_WINDOW: {
        int32_t key = 0;
        r_i32(&r, &key);
        if (key) g_window = key;
        printf("[host]    AddWindow key=%d -> resize + focus\n", g_window);
        if (g_w && g_h) send_resize(g_w, g_h);
        send_focus(1);
        break;
    }
    case VIEWHOST_SET_WINDOW_PROPERTIES: {
        // int32 window key, then a Pickle string (int32 length + padded bytes)
        int32_t key = 0, len = 0;
        r_i32(&r, &key);
        if (r_i32(&r, &len) && len > 0 && r.pos + (size_t)len <= paylen)
            printf("[host]    key=%d properties: %.*s\n", key,
                   (int)(len > 220 ? 220 : len), (const char *)pay + r.pos);
        else
            printf("[host]    key=%d (unparsed properties, %zu bytes)\n", key, paylen);
        break;
    }
    case VIEWHOST_SET_ORIENTATION: {
        int32_t o = 0; r_i32(&r, &o);
        printf("[host]    SetOrientation(%d)\n", o);
        break;
    }
    default:
        break;
    }

    // Any sync message must get a reply or the app blocks forever.
    if ((h->flags & SYNC_BIT) && !(h->flags & REPLY_BIT)) {
        Buf p = {0};
        w_i32(&p, 0);   // single out-param, zero is a safe default
        send_msg(h->routing, h->type, REPLY_BIT, h->id, &p);
        free(p.b);
        printf("[host]    (auto-replied to sync %s)\n", name);
    }
    fflush(stdout);
}

// ---------------------------------------------------------------------- main

static int read_full(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, (char *)buf + got, len - got, 0);
        if (n == 0) return 0;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)n;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "sysmgr";
    const char *ctl  = argc > 2 ? argv[2] : "/tmp/pdkhost.ctl";

    char path[108];
    snprintf(path, sizeof(path), "/tmp/pipcserver.%s", name);
    unlink(path);

    int sfd = socket(PF_LOCAL, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_LOCAL;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(sfd, (struct sockaddr *)&addr, SUN_LEN(&addr)) != 0) { perror("bind"); return 1; }
    chmod(path, 0666);
    if (listen(sfd, 16) != 0) { perror("listen"); return 1; }

    unlink(ctl);
    if (mkfifo(ctl, 0666) != 0 && errno != EEXIST) perror("mkfifo");
    int cfifo = open(ctl, O_RDONLY | O_NONBLOCK);

    printf("[host] listening on %s\n", path);
    printf("[host] control fifo %s  (try: echo 'key 32' > %s)\n", ctl, ctl);
    fflush(stdout);

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        g_client = cfd;

        char hello[256];
        if (read_full(cfd, hello, sizeof(hello)) != 1) {
            printf("[host] client vanished before hello\n");
            close(cfd); g_client = -1; continue;
        }
        int32_t pid; memcpy(&pid, hello, sizeof(pid));
        hello[sizeof(hello) - 1] = '\0';
        printf("[host] client connected: pid=%d name=\"%s\"\n", pid, hello + sizeof(pid));
        fflush(stdout);

        for (;;) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(cfd, &rf);
            int maxfd = cfd;
            if (cfifo >= 0) { FD_SET(cfifo, &rf); if (cfifo > maxfd) maxfd = cfifo; }

            if (select(maxfd + 1, &rf, NULL, NULL, NULL) < 0) {
                if (errno == EINTR) continue;
                perror("select"); break;
            }

            if (cfifo >= 0 && FD_ISSET(cfifo, &rf)) {
                char line[256];
                ssize_t n = read(cfifo, line, sizeof(line) - 1);
                if (n > 0) {
                    line[n] = '\0';
                    char *save = NULL;
                    for (char *l = strtok_r(line, "\n", &save); l;
                         l = strtok_r(NULL, "\n", &save))
                        handle_control_line(l);
                } else {
                    // writer closed; reopen so the fifo stays usable
                    close(cfifo);
                    cfifo = open(ctl, O_RDONLY | O_NONBLOCK);
                }
            }

            if (!FD_ISSET(cfd, &rf)) continue;

            int32_t len;
            int r = read_full(cfd, &len, sizeof(len));
            if (r == 0) { printf("[host] client disconnected\n"); break; }
            if (r < 0)  { perror("recv"); break; }
            if (len < (int32_t)sizeof(struct Header) || len > (1 << 20)) {
                printf("[host] bad frame length %d\n", len); break;
            }

            unsigned char *buf = malloc((size_t)len);
            if (read_full(cfd, buf, (size_t)len) != 1) { free(buf); break; }

            struct Header h;
            memcpy(&h, buf, sizeof(h));
            on_message(&h, buf + sizeof(h), (size_t)len - sizeof(h));
            free(buf);
        }

        close(cfd);
        g_client = -1;
        g_window = 0;
        printf("[host] waiting for next client\n");
        fflush(stdout);
    }
}
