#include "hostcaps.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#include <setjmp.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define CAP_UNKNOWN (-1)

static int used_probe;

void hv_host_note_probe(void) { used_probe = 1; }

#if defined(__APPLE__)

static int sysctl_flag(const char *name) {
    int value = 0;
    size_t len = sizeof value;
    if (sysctlbyname(name, &value, &len, NULL, 0) != 0) return CAP_UNKNOWN;
    return value != 0;
}

/* The definitive answer for a host whose sysctls do not name the feature: run
   the instruction and see whether it traps. The SIGILL handler is installed
   for the duration of the probe and put back afterwards; hv_host_has_lse is
   first called while the machine is being built, before any other thread or
   signal handler exists. */
static sigjmp_buf probe_env;
static volatile sig_atomic_t probe_faulted;

static void probe_handler(int sig) {
    (void)sig;
    probe_faulted = 1;
    siglongjmp(probe_env, 1);
}

/* Runs one instruction whose only memory operand is [x9] and whose source
   register is w10; returns 1 if it executed, 0 if it took SIGILL. */
static int runs_instruction(uint32_t insn, uint32_t *mem) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGILL, &sa, &old) != 0) return CAP_UNKNOWN;
    probe_faulted = 0;
    if (sigsetjmp(probe_env, 1) == 0) {
        __asm__ volatile(
            "mov x9, %0\n"
            "mov w10, #1\n"
            ".inst %1\n"
            "dmb sy\n" :: "r"(mem), "i"(insn) : "x9", "w10", "memory");
    }
    sigaction(SIGILL, &old, NULL);
    return probe_faulted ? 0 : 1;
}

static uint32_t probe_mem[2];

/* LDADD w10, w10, [x9]: 0xb8200000 | Rs<<16 | Rn<<5 | Rt. */
static int probe_lse(void) {
    return runs_instruction(0xb8200000u | 10u << 16 | 9u << 5 | 10u, probe_mem);
}

/* LDAPUR w10, [x9]: 0x19400000 | Rn<<5 | Rt. */
static int probe_lrcpc2(void) {
    return runs_instruction(0x19400000u | 9u << 5 | 10u, probe_mem);
}

static int probe_none(void) { return CAP_UNKNOWN; }

#else /* not Darwin: the Mac build and the tests always run on a modern core. */

static int probe_lse(void) { return CAP_UNKNOWN; }
static int probe_lrcpc2(void) { return CAP_UNKNOWN; }
static int probe_none(void) { return CAP_UNKNOWN; }

#endif

/* The first sysctl that answers decides; with none answering, the probe does;
   a host that answers neither way is assumed modern, so nothing is slowed
   down on a machine we know nothing about. */
static int resolve(const char *const *names, int nnames, int (*probe)(void), int assume) {
    for (int i = 0; i < nnames; i++) {
#if defined(__APPLE__)
        int v = sysctl_flag(names[i]);
        if (v != CAP_UNKNOWN) return v;
#endif
    }
    (void)names;
    (void)nnames;
    int v = probe();
    if (v == CAP_UNKNOWN) return assume;
    hv_host_note_probe();
    return v;
}

int hv_host_has_lse(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) {
        static const char *const names[] = {"hw.optional.arm.FEAT_LSE", "hw.optional.armv8_1_atomics"};
        cached = resolve(names, 2, probe_lse, 1);
    }
    return cached;
}

int hv_host_has_lrcpc2(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) {
        static const char *const names[] = {"hw.optional.arm.FEAT_LRCPC2", "hw.optional.armv8_3"};
        cached = resolve(names, 2, probe_lrcpc2, 1);
    }
    return cached;
}

int hv_host_has_dotprod(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) {
        static const char *const names[] = {"hw.optional.arm.FEAT_DotProd", "hw.optional.armv8_2_sha3"};
        cached = resolve(names, 2, probe_none, 1);
    }
    return cached;
}

int hv_host_has_fp16(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) {
        static const char *const names[] = {"hw.optional.arm.FEAT_FP16", "hw.optional.neon_fp16"};
        cached = resolve(names, 2, probe_none, 1);
    }
    return cached;
}

const char *hv_host_feature_summary(void) {
    static char buf[160];
    if (buf[0] == '\0') {
        snprintf(buf, sizeof buf, "LSE=%s LRCPC2=%s DotProd=%s FP16=%s%s",
                 hv_host_has_lse() ? "yes" : "no",
                 hv_host_has_lrcpc2() ? "yes" : "no",
                 hv_host_has_dotprod() ? "yes" : "no",
                 hv_host_has_fp16() ? "yes" : "no",
                 used_probe ? " (probed)" : "");
    }
    return buf;
}
