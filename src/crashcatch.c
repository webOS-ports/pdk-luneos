// crashcatch - LD_PRELOAD fault reporter for legacy PDK games.
//
// The qemu gdbstub is unusably slow for these titles (they need ~60s of emulated
// startup before they fault), so instead we catch the signal in-process at full
// speed and print the faulting address plus a backtrace with dladdr() resolution.
// That gives library + offset, which is enough to identify the culprit.
//
// Build:  see the crashcatch target in the Makefile
// Use:    -E LD_PRELOAD=/usr/lib/crashcatch.so

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <ucontext.h>

#define MAXFRAMES 24

static void write_str(const char *s)
{
    ssize_t r = write(STDERR_FILENO, s, strlen(s));
    (void)r;
}

static void write_hex(const char *label, unsigned long v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s0x%08lx\n", label, v);
    write_str(buf);
}

static const char *signame(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (bad memory access)";
    case SIGFPE:  return "SIGFPE (arithmetic error, usually divide by zero)";
    case SIGBUS:  return "SIGBUS (misaligned or invalid access)";
    case SIGILL:  return "SIGILL (illegal instruction)";
    case SIGABRT: return "SIGABRT (abort)";
    default:      return "signal";
    }
}

static void handler(int sig, siginfo_t *info, void *ucv)
{
    char buf[256];

    write_str("\n================ crashcatch ================\n");
    snprintf(buf, sizeof(buf), "signal : %d %s\n", sig, signame(sig));
    write_str(buf);
    // si_addr via the portable member
    if (info) write_hex("si_addr: ", (unsigned long)info->si_addr);

#if defined(__arm__)
    if (ucv) {
        ucontext_t *uc = (ucontext_t *)ucv;
        write_hex("pc     : ", (unsigned long)uc->uc_mcontext.arm_pc);
        write_hex("lr     : ", (unsigned long)uc->uc_mcontext.arm_lr);
        write_hex("sp     : ", (unsigned long)uc->uc_mcontext.arm_sp);
        write_hex("r0     : ", (unsigned long)uc->uc_mcontext.arm_r0);
        write_hex("r1     : ", (unsigned long)uc->uc_mcontext.arm_r1);

        // which object owns the faulting pc?
        Dl_info di;
        if (dladdr((void *)uc->uc_mcontext.arm_pc, &di) && di.dli_fname) {
            snprintf(buf, sizeof(buf), "in     : %s + 0x%lx\n", di.dli_fname,
                     (unsigned long)((char *)uc->uc_mcontext.arm_pc - (char *)di.dli_fbase));
            write_str(buf);
            if (di.dli_sname) {
                snprintf(buf, sizeof(buf), "symbol : %s\n", di.dli_sname);
                write_str(buf);
            }
        }
    }
#endif

    write_str("\n--- backtrace ---\n");
    void *frames[MAXFRAMES];
    int n = backtrace(frames, MAXFRAMES);
    for (int i = 0; i < n; i++) {
        Dl_info di;
        if (dladdr(frames[i], &di) && di.dli_fname) {
            snprintf(buf, sizeof(buf), "#%-2d %p  %s + 0x%lx%s%s\n", i, frames[i],
                     di.dli_fname,
                     (unsigned long)((char *)frames[i] - (char *)di.dli_fbase),
                     di.dli_sname ? "  " : "",
                     di.dli_sname ? di.dli_sname : "");
        } else {
            snprintf(buf, sizeof(buf), "#%-2d %p  ?\n", i, frames[i]);
        }
        write_str(buf);
    }
    write_str("============================================\n");

    _exit(128 + sig);
}

__attribute__((constructor))
static void install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    if (getenv("CRASHCATCH_QUIET") == NULL)
        write_str("[crashcatch] fault handlers installed\n");
}
