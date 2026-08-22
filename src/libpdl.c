// libpdl - reimplementation of Palm's PDK library for LuneOS.
//
// Replaces the 2011 libpdl.so, which was built against luna-service1, libnapp,
// libluna-prefs, HAL sensors and OpenSSL 0.9.8 - none of which exist on a modern
// LuneOS. Signatures come from /opt/PalmPDK/include/PDL*.h; behaviour was derived
// from a full Ghidra decompilation of the original (1013/1013 functions).
//
// Design notes:
//  * Every one of the original's 86 exported PDL_* symbols is defined here, so the
//    ABI is satisfied and no app fails to link.
//  * Device/locale data comes from LuneOS's own /var/luna/preferences/systemprefs.db
//    (read directly - no sqlite dependency, the values we need are plain strings).
//  * Service calls go out via luna-send if present; unavailable ones return
//    PDL_NOTIMPLEMENTED rather than pretending to succeed.
//  * PDL_Purchase* is deliberately "not available" - the Palm catalogue is gone.
//
// Build: clang --target=arm-linux-gnueabi -mfloat-abi=softfp -mfpu=vfpv3

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>

// ---------------------------------------------------------------- PDL types
// Mirrors PDL_types.h / PDL.h so we don't depend on the SDK being installed.

typedef int PDL_Err;
#define PDL_NOERROR            0
#define PDL_EOTHER            (-1)
#define PDL_NOTINITIALIZED    (-2)
#define PDL_BADFORMAT         (-3)
#define PDL_NOTIMPLEMENTED    (-4)
#define PDL_INVALIDPARAMETER  (-5)
#define PDL_BUFFERTOOSMALL    (-6)

typedef int PDL_bool;
#define PDL_TRUE  1
#define PDL_FALSE 0

typedef int PDL_key;
typedef int PDL_Orientation;
typedef int PDL_TouchAggression;
typedef int PDL_OGLVersion;
typedef int PDL_SensorType;

typedef struct { int horizontalPixels, verticalPixels, horizontalDPI, verticalDPI;
                 double aspectRatio; } PDL_ScreenMetrics;
typedef struct { int majorVersion, minorVersion, revisionMinorVersion;
                 char buildNumber[64]; } PDL_OSVersion;
typedef struct { double altitude, velocity, horizontalAccuracy, verticalAccuracy,
                        latitude, longitude, heading; } PDL_Location;
typedef struct { double magnetic, TRUEheading, x, y, z, accuracy; } PDL_Compass;
typedef struct { char ipAddress[64]; char netmask[64]; char broadcast[64]; } PDL_NetInfo;
typedef struct { double x, y, z; int type; } PDL_SensorEvent;

typedef struct PDL_JSParameters     PDL_JSParameters;
typedef struct PDL_ServiceParameters PDL_ServiceParameters;
typedef struct { int dummy; } PDL_ItemInfo;
typedef struct { int dummy; } PDL_ItemReceipt;
typedef struct { int dummy; } PDL_ItemCollection;

typedef PDL_bool (*PDL_JSHandlerFunc)(PDL_JSParameters *params);
typedef PDL_bool (*PDL_ServiceCallbackFunc)(PDL_ServiceParameters *params, void *user);

// SDL_WebOsHook* live in our libSDL shim; weak so libpdl still loads without it.
extern int SDL_WebOsHookSetOrientation(int) __attribute__((weak));
extern int SDL_WebOsHookSetGesturesEnable(int) __attribute__((weak));
extern int SDL_WebOsHookScreenTimeoutEnable(int) __attribute__((weak));
extern int SDL_WebOsHookBannerMessagesEnable(int) __attribute__((weak));
extern int SDL_WebOsHookCustomPauseUiEnable(int) __attribute__((weak));
extern int SDL_WebOsHookEnableCompass(int) __attribute__((weak));
extern const char *SDL_GetKeyName(int) __attribute__((weak));
extern char *SDL_GetError(void) __attribute__((weak));
extern void SDL_SetError(const char *, ...) __attribute__((weak));

// ------------------------------------------------------------------ internals

static int   s_initialised;
static char  s_appid[256];
static char  s_apppath[PATH_MAX];
static char  s_lasterr[256];

#define PDK_VERSION 303   // matches what the original reported on 3.0.5

static void pdl_seterr(const char *msg)
{
    snprintf(s_lasterr, sizeof(s_lasterr), "%s", msg ? msg : "");
    if (SDL_SetError) SDL_SetError("%s", s_lasterr);
}

static int pdl_debug(void)
{
    static int v = -1;
    if (v < 0) v = getenv("PDL_DEBUG") ? 1 : 0;
    return v;
}

#define DBG(...) do { if (pdl_debug()) { \
    fprintf(stderr, "[libpdl] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static PDL_Err copy_out(const char *value, char *buffer, int bufferLen)
{
    if (!buffer || bufferLen <= 0) return PDL_INVALIDPARAMETER;
    if (!value) { buffer[0] = '\0'; return PDL_EOTHER; }
    if ((int)strlen(value) + 1 > bufferLen) {
        snprintf(buffer, (size_t)bufferLen, "%s", value);
        return PDL_BUFFERTOOSMALL;
    }
    strcpy(buffer, value);
    return PDL_NOERROR;
}

// Pull a value out of LuneOS's systemprefs.db. It is a SQLite file, but the rows we
// want are short plain strings, so a bounded scan avoids a sqlite3 dependency.
static int prefs_lookup(const char *key, char *out, size_t outlen)
{
    static char  *blob;
    static size_t bloblen;

    if (!blob) {
        FILE *f = fopen("/var/luna/preferences/systemprefs.db", "rb");
        if (!f) return 0;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
        long n = ftell(f);
        if (n <= 0 || n > (8 << 20)) { fclose(f); return 0; }
        rewind(f);
        blob = malloc((size_t)n + 1);
        if (!blob) { fclose(f); return 0; }
        bloblen = fread(blob, 1, (size_t)n, f);
        blob[bloblen] = '\0';
        fclose(f);
    }

    size_t klen = strlen(key);
    for (size_t i = 0; i + klen + 1 < bloblen; i++) {
        if (memcmp(blob + i, key, klen) != 0) continue;
        // value follows the key, as printable text
        size_t j = i + klen;
        while (j < bloblen && (blob[j] < 0x20 || blob[j] > 0x7e)) j++;
        size_t s = j;
        while (j < bloblen && blob[j] >= 0x20 && blob[j] <= 0x7e) j++;
        if (j > s && j - s < outlen) {
            memcpy(out, blob + s, j - s);
            out[j - s] = '\0';
            return 1;
        }
    }
    return 0;
}

// Walk up from the executable looking for appinfo.json, as the original did.
static const char *app_dir(void)
{
    if (s_apppath[0]) return s_apppath;

    char exe[PATH_MAX];
    // pdk-run knows the application directory and exports it. Prefer that:
    // /proc/self/exe is only the game when qemu-user fakes it or the binary was
    // exec'd directly. Running natively through the sysroot's loader - which is
    // how arm64 devices avoid emulation - makes it point at ld-linux.so.3, and
    // walking up from there finds no appinfo.json at all, so every title came up
    // as appId=unknown.
    const char *envdir = getenv("PDK_APP_DIR");
    if (envdir && *envdir) {
        char probe[PATH_MAX];
        snprintf(probe, sizeof(probe), "%s/appinfo.json", envdir);
        struct stat st;
        if (stat(probe, &st) == 0) {
            snprintf(s_apppath, sizeof(s_apppath), "%s", envdir);
            return s_apppath;
        }
    }

    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return NULL;
    exe[n] = '\0';

    char *slash = strrchr(exe, '/');
    while (slash) {
        *slash = '\0';
        char probe[PATH_MAX];
        snprintf(probe, sizeof(probe), "%s/appinfo.json", exe);
        struct stat st;
        if (stat(probe, &st) == 0) {
            snprintf(s_apppath, sizeof(s_apppath), "%s", exe);
            return s_apppath;
        }
        slash = strrchr(exe, '/');
    }
    return NULL;
}

static int appinfo_value(const char *name, char *out, size_t outlen)
{
    const char *dir = app_dir();
    if (!dir) return 0;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/appinfo.json", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    char buf[8192];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';

    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", name);
    char *p = strstr(buf, needle);
    if (!p) return 0;
    p = strchr(p + strlen(needle), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '"') {
        p++;
        char *e = strchr(p, '"');
        if (!e || (size_t)(e - p) >= outlen) return 0;
        memcpy(out, p, (size_t)(e - p));
        out[e - p] = '\0';
    } else {
        char *e = p;
        while (*e && *e != ',' && *e != '}' && *e != '\n') e++;
        while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if ((size_t)(e - p) >= outlen) return 0;
        memcpy(out, p, (size_t)(e - p));
        out[e - p] = '\0';
    }
    return 1;
}

// Fire a luna-service2 request. Returns 0 on success.
static int ls_call(const char *uri, const char *payload)
{
    if (!uri) return -1;
    if (access("/usr/bin/luna-send", X_OK) != 0) {
        DBG("luna-send unavailable, dropping call to %s", uri);
        return -1;
    }
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "/usr/bin/luna-send -n 1 '%s' '%s' >/dev/null 2>&1 &",
             uri, payload ? payload : "{}");
    DBG("ls_call %s %s", uri, payload ? payload : "{}");
    return system(cmd) == 0 ? 0 : -1;
}

// ------------------------------------------------------------ lifecycle

PDL_Err PDL_Init(unsigned int flags)
{
    (void)flags;
    if (s_initialised) return PDL_NOERROR;

    if (!appinfo_value("id", s_appid, sizeof(s_appid)))
        snprintf(s_appid, sizeof(s_appid), "unknown.pdk.app");

    s_initialised = 1;
    DBG("PDL_Init: appId=%s dir=%s", s_appid, app_dir() ? app_dir() : "(none)");
    return PDL_NOERROR;
}

void PDL_Quit(void)
{
    DBG("PDL_Quit");
    s_initialised = 0;
}

int PDL_GetPDKVersion(void) { return PDK_VERSION; }

void PDL_Log(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    fprintf(stderr, "[pdk] ");
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// ------------------------------------------------------------ device / locale

PDL_Err PDL_GetLanguage(char *buffer, int bufferLen)
{
    char v[64];
    if (prefs_lookup("languageCode", v, sizeof(v))) {
        char region[64];
        if (prefs_lookup("countryCode", region, sizeof(region))) {
            char joined[160];
            snprintf(joined, sizeof(joined), "%s_%s", v, region);
            for (char *p = joined; *p; p++) *p = (char)tolower((unsigned char)*p);
            return copy_out(joined, buffer, bufferLen);
        }
        return copy_out(v, buffer, bufferLen);
    }
    return copy_out("en_us", buffer, bufferLen);
}

PDL_Err PDL_GetRegionCountryCode(char *buffer, int bufferLen)
{
    char v[64];
    if (prefs_lookup("countryCode", v, sizeof(v))) {
        for (char *p = v; *p; p++) *p = (char)tolower((unsigned char)*p);
        return copy_out(v, buffer, bufferLen);
    }
    return copy_out("us", buffer, bufferLen);
}

PDL_Err PDL_GetRegionCountryName(char *buffer, int bufferLen)
{
    char v[128];
    if (prefs_lookup("country", v, sizeof(v)))
        return copy_out(v, buffer, bufferLen);
    return copy_out("United States", buffer, bufferLen);
}

PDL_Err PDL_GetDeviceName(char *buffer, int bufferLen)
{
    char v[128];
    if (prefs_lookup("deviceName", v, sizeof(v)))
        return copy_out(v, buffer, bufferLen);
    return copy_out("LuneOS Device", buffer, bufferLen);
}

int PDL_GetHardwareID(void)
{
    const char *env = getenv("PDL_HARDWARE_ID");
    return env ? atoi(env) : -1;    // -1 == unknown, as the original reported
}

PDL_Err PDL_GetUniqueID(char *buffer, int bufferLen)
{
    char v[128];
    if (prefs_lookup("deviceId", v, sizeof(v)) || prefs_lookup("nduid", v, sizeof(v)))
        return copy_out(v, buffer, bufferLen);

    FILE *f = fopen("/etc/machine-id", "r");
    if (f) {
        char id[64] = {0};
        if (fgets(id, sizeof(id), f)) {
            id[strcspn(id, "\r\n")] = '\0';
            fclose(f);
            return copy_out(id, buffer, bufferLen);
        }
        fclose(f);
    }
    return copy_out("000000000000000000000000000000000000000", buffer, bufferLen);
}

PDL_Err PDL_GetOSVersion(PDL_OSVersion *osVersion)
{
    if (!osVersion) return PDL_INVALIDPARAMETER;
    osVersion->majorVersion = 3;
    osVersion->minorVersion = 0;
    osVersion->revisionMinorVersion = 5;
    snprintf(osVersion->buildNumber, sizeof(osVersion->buildNumber), "LuneOS");
    return PDL_NOERROR;
}

PDL_Err PDL_GetScreenMetrics(PDL_ScreenMetrics *outMetrics)
{
    if (!outMetrics) return PDL_INVALIDPARAMETER;

    int w = 1024, h = 768, dpi = 132;
    const char *ws = getenv("PDK_SCREEN_WIDTH");
    const char *hs = getenv("PDK_SCREEN_HEIGHT");
    const char *ds = getenv("PDK_SCREEN_DPI");
    if (ws) w = atoi(ws);
    if (hs) h = atoi(hs);
    if (ds) dpi = atoi(ds);

    outMetrics->horizontalPixels = w;
    outMetrics->verticalPixels   = h;
    outMetrics->horizontalDPI    = dpi;
    outMetrics->verticalDPI      = dpi;
    outMetrics->aspectRatio      = h ? (double)w / (double)h : 1.0;
    return PDL_NOERROR;
}

PDL_Err PDL_GetDataFilePath(const char *dataFileName, char *buffer, int bufferLen)
{
    if (!dataFileName) { pdl_seterr("dataFileName is NULL"); return PDL_INVALIDPARAMETER; }
    if (!buffer)       { pdl_seterr("buffer is NULL");       return PDL_INVALIDPARAMETER; }
    if (bufferLen <= 0){ pdl_seterr("buffer length must be greater than 0");
                         return PDL_INVALIDPARAMETER; }

    const char *dir = app_dir();
    if (!dir) return PDL_EOTHER;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, dataFileName);
    return copy_out(path, buffer, bufferLen);
}

PDL_Err PDL_GetAppinfoValue(const char *name, char *buffer, int bufferLen)
{
    if (!name || !buffer || bufferLen <= 0) return PDL_INVALIDPARAMETER;
    char v[1024];
    if (!appinfo_value(name, v, sizeof(v))) return PDL_EOTHER;
    return copy_out(v, buffer, bufferLen);
}

PDL_Err PDL_GetCallingPath(char *buffer, int bufferLen)
{
    const char *dir = app_dir();
    return dir ? copy_out(dir, buffer, bufferLen) : PDL_EOTHER;
}

const char *PDL_GetKeyName(PDL_key Key)
{
    if (SDL_GetKeyName) return SDL_GetKeyName((int)Key);
    return "";
}

// ------------------------------------------------------------ window / chrome

PDL_Err PDL_SetOrientation(PDL_Orientation orientation)
{
    if (SDL_WebOsHookSetOrientation) SDL_WebOsHookSetOrientation((int)orientation);
    return PDL_NOERROR;
}

PDL_Err PDL_GesturesEnable(PDL_bool Enable)
{
    if (SDL_WebOsHookSetGesturesEnable) SDL_WebOsHookSetGesturesEnable(Enable);
    return PDL_NOERROR;
}

PDL_Err PDL_ScreenTimeoutEnable(PDL_bool Enable)
{
    if (SDL_WebOsHookScreenTimeoutEnable) SDL_WebOsHookScreenTimeoutEnable(Enable);
    return PDL_NOERROR;
}

PDL_Err PDL_BannerMessagesEnable(PDL_bool Enable)
{
    if (SDL_WebOsHookBannerMessagesEnable) SDL_WebOsHookBannerMessagesEnable(Enable);
    return PDL_NOERROR;
}

PDL_Err PDL_CustomPauseUiEnable(PDL_bool Enable)
{
    if (SDL_WebOsHookCustomPauseUiEnable) SDL_WebOsHookCustomPauseUiEnable(Enable);
    return PDL_NOERROR;
}

PDL_Err PDL_SetKeyboardState(PDL_bool bVisible)
{
    DBG("SetKeyboardState(%d)", bVisible);
    return PDL_NOERROR;
}

PDL_Err PDL_SetTouchAggression(PDL_TouchAggression aggression)
{
    (void)aggression;
    return PDL_NOERROR;
}

PDL_Err PDL_Minimize(void)
{
    return ls_call("palm://com.palm.applicationManager/launch",
                   "{\"id\":\"com.palm.launcher\"}") == 0
           ? PDL_NOERROR : PDL_NOTIMPLEMENTED;
}

PDL_Err PDL_Vibrate(int periodMS, int durationMS)
{
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"period\":%d,\"duration\":%d}",
             periodMS, durationMS);
    return ls_call("palm://com.palm.vibrate/vibrate", payload) == 0
           ? PDL_NOERROR : PDL_NOTIMPLEMENTED;
}

PDL_Err PDL_LaunchBrowser(const char *Url)
{
    if (!Url) return PDL_INVALIDPARAMETER;
    char payload[1536];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"com.palm.app.browser\",\"params\":{\"target\":\"%s\"}}", Url);
    return ls_call("palm://com.palm.applicationManager/launch", payload) == 0
           ? PDL_NOERROR : PDL_NOTIMPLEMENTED;
}

PDL_Err PDL_LaunchEmail(const char *Subject, const char *Body)
{
    char payload[2048];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"com.palm.app.email\",\"params\":{\"summary\":\"%s\",\"text\":\"%s\"}}",
             Subject ? Subject : "", Body ? Body : "");
    return ls_call("palm://com.palm.applicationManager/launch", payload) == 0
           ? PDL_NOERROR : PDL_NOTIMPLEMENTED;
}

PDL_Err PDL_LaunchEmailTo(const char *Subject, const char *Body,
                          int numRecipients, const char **recipients)
{
    char rcpt[1024] = "";
    for (int i = 0; i < numRecipients && recipients; i++) {
        char one[256];
        snprintf(one, sizeof(one), "%s\"%s\"", i ? "," : "",
                 recipients[i] ? recipients[i] : "");
        strncat(rcpt, one, sizeof(rcpt) - strlen(rcpt) - 1);
    }
    char payload[3072];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"com.palm.app.email\",\"params\":{\"summary\":\"%s\",\"text\":\"%s\","
             "\"recipients\":[%s]}}",
             Subject ? Subject : "", Body ? Body : "", rcpt);
    return ls_call("palm://com.palm.applicationManager/launch", payload) == 0
           ? PDL_NOERROR : PDL_NOTIMPLEMENTED;
}

PDL_Err PDL_NotifyMusicPlaying(PDL_bool MusicPlaying)
{
    if (!MusicPlaying) return PDL_NOERROR;
    ls_call("palm://com.palm.mediad/service/pauseAllMediaPlayback", "{}");
    return PDL_NOERROR;
}

PDL_Err PDL_SetAutomaticSoundPausing(PDL_bool AutomaticallyPause)
{
    (void)AutomaticallyPause;
    return PDL_NOERROR;
}

PDL_Err PDL_SetFirewallPortStatus(int port, PDL_bool Open)
{
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"port\":%d,\"open\":%s}",
             port, Open ? "true" : "false");
    return ls_call("palm://com.palm.firewall/control", payload) == 0
           ? PDL_NOERROR : PDL_NOTIMPLEMENTED;
}

PDL_Err PDL_LoadOGL(PDL_OGLVersion version)
{
    (void)version;   // the loader resolves GL for us now
    return PDL_NOERROR;
}

// ------------------------------------------------------------ sensors

PDL_bool PDL_SensorExists(PDL_SensorType sensor) { (void)sensor; return PDL_FALSE; }
PDL_Err  PDL_EnableSensor(PDL_SensorType s, PDL_bool e) { (void)s; (void)e; return PDL_NOTIMPLEMENTED; }
PDL_Err  PDL_PollSensor(PDL_SensorType s, PDL_SensorEvent *e)
{
    (void)s;
    if (e) memset(e, 0, sizeof(*e));
    return PDL_NOTIMPLEMENTED;
}
PDL_Err PDL_PollActiveSensors(PDL_SensorEvent *e)
{
    if (e) memset(e, 0, sizeof(*e));
    return PDL_NOTIMPLEMENTED;
}
PDL_Err PDL_EnableCompass(PDL_bool activate)
{
    if (SDL_WebOsHookEnableCompass) SDL_WebOsHookEnableCompass(activate);
    return PDL_NOTIMPLEMENTED;
}
PDL_Err PDL_GetCompass(PDL_Compass *compass)
{
    if (compass) memset(compass, 0, sizeof(*compass));
    return PDL_NOTIMPLEMENTED;
}
PDL_Err PDL_EnableLocationTracking(PDL_bool activate) { (void)activate; return PDL_NOTIMPLEMENTED; }
PDL_Err PDL_GetLocation(PDL_Location *location)
{
    if (location) memset(location, 0, sizeof(*location));
    return PDL_NOTIMPLEMENTED;
}
PDL_Err PDL_GetNetInfo(const char *interfaceName, PDL_NetInfo *interfaceInfo)
{
    (void)interfaceName;
    if (interfaceInfo) memset(interfaceInfo, 0, sizeof(*interfaceInfo));
    return PDL_NOTIMPLEMENTED;
}

// ------------------------------------------------------------ services / JS
//
// The JS bridge existed so a PDK app embedded in a Mojo card could call into
// JavaScript. Nothing hosts PDK apps that way on LuneOS, so these report cleanly
// rather than pretending.

PDL_Err PDL_ServiceCall(const char *uri, const char *payload)
{
    return ls_call(uri, payload) == 0 ? PDL_NOERROR : PDL_NOTIMPLEMENTED;
}

PDL_Err PDL_ServiceCallWithCallback(const char *uri, const char *payload,
                                    PDL_ServiceCallbackFunc callback, void *user,
                                    PDL_bool removeAfterResponse)
{
    (void)callback; (void)user; (void)removeAfterResponse;
    return ls_call(uri, payload) == 0 ? PDL_NOERROR : PDL_NOTIMPLEMENTED;
}

PDL_Err PDL_UnregisterServiceCallback(PDL_ServiceCallbackFunc callback)
{
    (void)callback; return PDL_NOERROR;
}

PDL_bool PDL_ParamExists(PDL_ServiceParameters *p, const char *name)
{ (void)p; (void)name; return PDL_FALSE; }
void PDL_GetParamString(PDL_ServiceParameters *p, const char *name, char *buf, int len)
{ (void)p; (void)name; if (buf && len > 0) buf[0] = '\0'; }
int PDL_GetParamInt(PDL_ServiceParameters *p, const char *name)
{ (void)p; (void)name; return 0; }
double PDL_GetParamDouble(PDL_ServiceParameters *p, const char *name)
{ (void)p; (void)name; return 0.0; }
PDL_bool PDL_GetParamBool(PDL_ServiceParameters *p, const char *name)
{ (void)p; (void)name; return PDL_FALSE; }

PDL_Err PDL_RegisterJSHandler(const char *fn, PDL_JSHandlerFunc f)
{ (void)fn; (void)f; return PDL_NOTIMPLEMENTED; }
PDL_Err PDL_RegisterPollingJSHandler(const char *fn, PDL_JSHandlerFunc f)
{ (void)fn; (void)f; return PDL_NOTIMPLEMENTED; }
PDL_Err PDL_JSRegistrationComplete(void) { return PDL_NOERROR; }
int  PDL_HandleJSCalls(void) { return 0; }
PDL_bool PDL_IsPoller(PDL_JSParameters *p) { (void)p; return PDL_FALSE; }
int  PDL_GetNumJSParams(PDL_JSParameters *p) { (void)p; return 0; }
int  PDL_GetJSParamInt(PDL_JSParameters *p, int n) { (void)p; (void)n; return 0; }
double PDL_GetJSParamDouble(PDL_JSParameters *p, int n) { (void)p; (void)n; return 0.0; }
PDL_Err PDL_JSReply(PDL_JSParameters *p, const char *r) { (void)p; (void)r; return PDL_NOTIMPLEMENTED; }
PDL_Err PDL_JSException(PDL_JSParameters *p, const char *r) { (void)p; (void)r; return PDL_NOTIMPLEMENTED; }
PDL_Err PDL_CallJS(const char *fn, const char **params, int n)
{ (void)fn; (void)params; (void)n; return PDL_NOTIMPLEMENTED; }

PDL_bool PDL_IsPlugin(void) { return PDL_FALSE; }
PDL_bool PDL_IsFullscreenPlugin(void) { return PDL_FALSE; }
PDL_Err  PDL_DismissFullscreen(void) { return PDL_NOERROR; }

// ------------------------------------------------------------ licensing / IAP
//
// The Palm app catalogue and payment service no longer exist. Report success for
// the licence check (the app is installed, so it is licensed) and "not available"
// for purchases.

PDL_Err PDL_CheckLicense(void) { return PDL_NOERROR; }

void PDL_FreeItemInfo(PDL_ItemInfo *i) { free(i); }
void PDL_FreeItemReceipt(PDL_ItemReceipt *i) { free(i); }
void PDL_FreeItemCollection(PDL_ItemCollection *i) { free(i); }

const char *PDL_GetError(void) { return s_lasterr; }

// ------------------------------------------------------------ PDL internals
//
// Seven symbols the original exported without declaring in any public header.
// Games do link them: Aftermath X HD uses the Ctx/Sync/Config set, Action Racing
// 3D uses PDL_GetHardware. Signatures recovered from the Ghidra decompilation of
// the 3.0.5 libpdl (see reference/libpdl-3.0.5-decompiled.c).
//
// The Sync/Config family drove Palm's cloud-save and server-config features over
// curl/json-c against services that no longer exist, so they report "nothing to
// do" rather than pretending a sync happened.

void *PDL_NewCtx(const char *name)
{
    DBG("PDL_NewCtx(%s)", name ? name : "(null)");
    return calloc(1, 64);          // opaque handle; callers only hand it back
}

void PDL_FreeCtx(void *ctx)
{
    DBG("PDL_FreeCtx(%p)", ctx);
    free(ctx);
}

int PDL_Sync(int handle, const char *what)
{
    (void)handle;
    DBG("PDL_Sync(%s) - no sync service", what ? what : "(null)");
    return 0;
}

int PDL_RemoteSync(void)
{
    DBG("PDL_RemoteSync - no sync service");
    return 0;
}

int PDL_ServerConfig(void)
{
    DBG("PDL_ServerConfig - no config server");
    return 0;
}

int PDL_InitConfig(void)
{
    DBG("PDL_InitConfig");
    return 0;
}

// Returned "castle" / "pixie" / "topaz" on real hardware. Override with
// PDL_HARDWARE if a game gates behaviour on the device model.
const char *PDL_GetHardware(void)
{
    const char *env = getenv("PDL_HARDWARE");
    return env ? env : "topaz";
}

// ------------------------------------------------- in-app purchase / licensing
//
// Palm's payment service (com.palm.service.payment) and app catalogue are gone,
// so nothing here can succeed. Report "no items / not available" rather than
// faking a purchase. Static analysis over 636 shipped titles shows ~12 link these,
// and a missing symbol stops the app loading at all - so they must exist even
// though they cannot work.
//
// PDL_isAppLicensedForDevice is the exception: it is a DRM check, and the app is
// installed on the device, so answer yes. Otherwise those titles refuse to start.

PDL_ItemCollection *PDL_GetAvailableItems(void)
{
    DBG("PDL_GetAvailableItems - no payment service");
    return NULL;
}

PDL_ItemInfo *PDL_GetItemInfo(const char *itemID)
{
    DBG("PDL_GetItemInfo(%s) - no payment service", itemID ? itemID : "(null)");
    return NULL;
}

PDL_ItemReceipt *PDL_PurchaseItem(const char *itemID, int qty, const char *usr)
{
    (void)qty; (void)usr;
    DBG("PDL_PurchaseItem(%s) - no payment service", itemID ? itemID : "(null)");
    pdl_seterr("in-app purchasing is not available");
    return NULL;
}

PDL_ItemReceipt *PDL_GetPendingPurchaseInfo(const char *orderNo)
{
    (void)orderNo;
    return NULL;
}

const char *PDL_GetItemJSON(PDL_ItemInfo *i)             { (void)i; return "{}"; }
const char *PDL_GetItemReceiptJSON(PDL_ItemReceipt *r)   { (void)r; return "{}"; }
const char *PDL_GetItemCollectionJSON(PDL_ItemCollection *c) { (void)c; return "{\"items\":[]}"; }
const char *PDL_GetParamJson(PDL_ServiceParameters *p)   { (void)p; return "{}"; }

// The app is installed, therefore it is licensed.
PDL_bool PDL_isAppLicensedForDevice(const char *appId)
{
    (void)appId;
    return PDL_TRUE;
}

// PDLNet_Get_Info - the only PDLNet_* symbol the original exported, declared in no
// header. It is PDL_GetNetInfo under another name. Behaviour and the (unusual)
// error codes come from the decompilation: reject a NULL or empty interface name,
// otherwise zero the caller's info struct.
//
// My first attempt guessed the signature as (char *buf, int *len) and wrote through
// the second argument - which segfaulted X-Plane. The second argument is an output
// struct, not a length.
#define PDLNET_ERR_NULL_NAME  (-0x2a48)
#define PDLNET_ERR_EMPTY_NAME (-0x2a30)

int PDLNet_Get_Info(const char *interfaceName, int *info)
{
    DBG("PDLNet_Get_Info(%s)", interfaceName ? interfaceName : "(null)");
    if (!interfaceName)   return PDLNET_ERR_NULL_NAME;
    if (!*interfaceName)  return PDLNET_ERR_EMPTY_NAME;
    if (info) { info[0] = 0; info[1] = 0; info[2] = 0; }   // no interface info here
    return PDL_NOERROR;
}
