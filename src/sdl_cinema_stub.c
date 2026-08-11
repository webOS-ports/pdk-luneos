// libSDL_cinema stub.
//
// Palm's libSDL_cinema played back intro/cutscene video by talking to the webOS
// media service through libmedia-api (com.palm.mediad over luna-service1). None of
// that exists on LuneOS, so the real library's CIN_Init() fails.
//
// That failure is not cosmetic. NFS Undercover's midp::RuntimePalm::initRuntimePalm()
// does:
//
//     bl    CIN_Init
//     cmp   r3, #0
//     movne r2, #0        ; r2 = (result == 0)  -- a logical NOT
//     moveq r2, #1
//     cmp   r1, #0
//     beq   continue      ; result != 0 -> carry on
//     b     bail          ; result == 0 -> return early, SILENTLY
//
// Note the polarity: this API returns NON-ZERO for success, so a stub that
// helpfully returns 0 reproduces the exact failure it was meant to avoid.
//
// and the bail path skips construction of DisplayPalm further down. The MIDlet's
// Display is then never registered, midp::Display::getDisplay() returns NULL, and
// MonkeyApp::startApp() makes a virtual call through it and segfaults. A silent
// video-init failure therefore presents as a null-pointer crash much later.
//
// This stub reports a successful init so startup proceeds, and refuses to load any
// clip so the game skips its videos rather than waiting for playback that will
// never finish.

#include <stdio.h>
#include <stdlib.h>

static int verbose(void)
{
    static int v = -1;
    if (v < 0) v = getenv("PDK_CINEMA_DEBUG") ? 1 : 0;
    return v;
}

#define TRACE(...) do { if (verbose()) { \
    fprintf(stderr, "[cinema-stub] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

// NON-ZERO == success (see the disassembly above). Callers abandon their entire
// startup if this returns 0.
int CIN_Init(void)
{
    TRACE("CIN_Init -> 1 (pretending video playback is available)");
    return 1;
}

int CIN_DeInit(void)
{
    TRACE("CIN_DeInit");
    return 0;
}

// Refuse the clip: callers treat this as "no video" and move on. Returning success
// here would leave them waiting on an end-of-stream that never arrives.
int CIN_LoadCIN(const char *path)
{
    TRACE("CIN_LoadCIN(%s) -> -1 (skipping video)", path ? path : "(null)");
    return -1;
}

int CIN_Play(void)
{
    TRACE("CIN_Play -> -1");
    return -1;
}

int CIN_Pause(void)
{
    TRACE("CIN_Pause");
    return 0;
}

int CIN_Stop(void)
{
    TRACE("CIN_Stop");
    return 0;
}
