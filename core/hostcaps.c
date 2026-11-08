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
   the instruction and see whether it traps. The SIGILL handler is installed for
   the duration of the probe and put back afterwards; these run while the machine
   is being built, before any other thread or signal handler exists.

   The instructions are literals because `.inst` needs an immediate. Each word
   comes from QEMU's decodetree pattern for that instruction (see core/simd.c),
   with the registers it needs filled in. */
static sigjmp_buf probe_env;
static volatile sig_atomic_t probe_faulted;
static volatile uint32_t probe_mem[4];

static void probe_handler(int sig) {
    (void)sig;
    probe_faulted = 1;
    siglongjmp(probe_env, 1);
}

#define PROBE_BEGIN()                                                     \
    struct sigaction sa, old;                                             \
    memset(&sa, 0, sizeof sa);                                            \
    sa.sa_handler = probe_handler;                                        \
    sigemptyset(&sa.sa_mask);                                             \
    if (sigaction(SIGILL, &sa, &old) != 0) return CAP_UNKNOWN;            \
    probe_faulted = 0;                                                    \
    if (sigsetjmp(probe_env, 1) == 0) {

#define PROBE_END()                                                       \
    }                                                                     \
    sigaction(SIGILL, &old, NULL);                                        \
    return probe_faulted ? 0 : 1;

#if defined(__arm64__)

/* ldadd w10, w10, [x9]: 0xb8200000 | Rs<<16 | Rn<<5 | Rt. */
static int probe_lse(void) {
    PROBE_BEGIN()
        __asm__ volatile("mov x9, %0\n"
                         "mov w10, #1\n"
                         ".inst 0xb82a012a\n"
                         "dmb sy\n" :: "r"((void *)probe_mem) : "x9", "w10", "memory");
    PROBE_END()
}

/* ldapur w10, [x9]: 0x19400000 | Rn<<5 | Rt. */
static int probe_lrcpc2(void) {
    PROBE_BEGIN()
        __asm__ volatile("mov x9, %0\n"
                         ".inst 0x1940012a\n"
                         "dmb sy\n" :: "r"((void *)probe_mem) : "x9", "w10", "memory");
    PROBE_END()
}

/* crc32b w10, w10, w10: 0x1ac04000 | Rm<<16 | Rn<<5 | Rd. */
static int probe_crc32(void) {
    PROBE_BEGIN()
        __asm__ volatile(".inst 0x1aca414a\n" ::: "w10", "cc");
    PROBE_END()
}

/* sqrdmlah v0.4h, v1.4h, v2.4h: 0x2e008400 | size<<22 | Rm<<16 | Rn<<5 | Rd. */
static int probe_rdm(void) {
    PROBE_BEGIN()
        __asm__ volatile(".inst 0x2e428420\n" ::: "v0", "v1", "v2");
    PROBE_END()
}

/* fadd v0.8h, v1.8h, v2.8h: 0x0e401400 | size<<22 | Rm<<16 | Rn<<5 | Rd, with the
   Q bit (30) set for the 128-bit form. */
static int probe_fp16(void) {
    PROBE_BEGIN()
        __asm__ volatile(".inst 0x4e421420\n" ::: "v0", "v1", "v2");
    PROBE_END()
}

/* fmlal v0.4s, v1.4h, v2.4h: 0x0e20ec00 | Rm<<16 | Rn<<5 | Rd. */
static int probe_fhm(void) {
    PROBE_BEGIN()
        __asm__ volatile(".inst 0x0e22ec20\n" ::: "v0", "v1", "v2");
    PROBE_END()
}

/* sdot v0.4s, v1.16b, v2.16b: 0x0e809400 | Rm<<16 | Rn<<5 | Rd. */
static int probe_dotprod(void) {
    PROBE_BEGIN()
        __asm__ volatile(".inst 0x0e829420\n" ::: "v0", "v1", "v2");
    PROBE_END()
}

/* eor3 v0.16b, v1.16b, v2.16b, v3.16b: 0xce000000 | Rm<<16 | Ra<<10 | Rn<<5 | Rd. */
static int probe_sha3(void) {
    PROBE_BEGIN()
        __asm__ volatile(".inst 0xce020c20\n" ::: "v0", "v1", "v2", "v3");
    PROBE_END()
}

/* sha512h q0, q1, v2.2d: 0xce608000 | Rm<<16 | Rn<<5 | Rd. */
static int probe_sha512(void) {
    PROBE_BEGIN()
        __asm__ volatile(".inst 0xce628020\n" ::: "v0", "v1", "v2");
    PROBE_END()
}

/* fcadd v0.4s, v1.4s, v2.4s, #90: 0x2e00e400 | size<<22 | Rm<<16 | Rn<<5 | Rd. */
static int probe_fcma(void) {
    PROBE_BEGIN()
        __asm__ volatile(".inst 0x2e82e420\n" ::: "v0", "v1", "v2");
    PROBE_END()
}

/* fjcvtzs w10, d10: 0x1e7e0000 | Rn<<5 | Rd. */
static int probe_jscvt(void) {
    PROBE_BEGIN()
        __asm__ volatile(".inst 0x1e7e014a\n" ::: "w10", "v10");
    PROBE_END()
}

#else /* an Apple host that is not arm64: nothing to probe with */

static int probe_lse(void) { return CAP_UNKNOWN; }
static int probe_lrcpc2(void) { return CAP_UNKNOWN; }
static int probe_crc32(void) { return CAP_UNKNOWN; }
static int probe_rdm(void) { return CAP_UNKNOWN; }
static int probe_fp16(void) { return CAP_UNKNOWN; }
static int probe_fhm(void) { return CAP_UNKNOWN; }
static int probe_dotprod(void) { return CAP_UNKNOWN; }
static int probe_sha3(void) { return CAP_UNKNOWN; }
static int probe_sha512(void) { return CAP_UNKNOWN; }
static int probe_fcma(void) { return CAP_UNKNOWN; }
static int probe_jscvt(void) { return CAP_UNKNOWN; }
#endif

#else /* not Darwin: the Mac build and the tests always run on a modern core. */

static int probe_lse(void) { return CAP_UNKNOWN; }
static int probe_lrcpc2(void) { return CAP_UNKNOWN; }
static int probe_crc32(void) { return CAP_UNKNOWN; }
static int probe_rdm(void) { return CAP_UNKNOWN; }
static int probe_fp16(void) { return CAP_UNKNOWN; }
static int probe_fhm(void) { return CAP_UNKNOWN; }
static int probe_dotprod(void) { return CAP_UNKNOWN; }
static int probe_sha3(void) { return CAP_UNKNOWN; }
static int probe_sha512(void) { return CAP_UNKNOWN; }
static int probe_fcma(void) { return CAP_UNKNOWN; }
static int probe_jscvt(void) { return CAP_UNKNOWN; }
#endif

/* The first sysctl that answers decides; with none answering, the probe does;
   a host that answers neither way is assumed modern, so nothing is slowed down
   on a machine we know nothing about. */
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

#define CAP(name, probe, ...)                                                     \
    do {                                                                          \
        static const char *const names[] = {__VA_ARGS__};                         \
        cached = resolve(names, (int)(sizeof names / sizeof names[0]), probe, 1);  \
    } while (0)

int hv_host_has_lse(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) CAP(lse, probe_lse, "hw.optional.arm.FEAT_LSE", "hw.optional.armv8_1_atomics");
    return cached;
}

int hv_host_has_lrcpc2(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) CAP(lrcpc2, probe_lrcpc2, "hw.optional.arm.FEAT_LRCPC2", "hw.optional.armv8_3");
    return cached;
}

int hv_host_has_crc32(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) CAP(crc32, probe_crc32, "hw.optional.arm.FEAT_CRC32", "hw.optional.armv8_crc32");
    return cached;
}

int hv_host_has_rdm(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) CAP(rdm, probe_rdm, "hw.optional.arm.FEAT_RDM");
    return cached;
}

int hv_host_has_fp16(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) CAP(fp16, probe_fp16, "hw.optional.arm.FEAT_FP16", "hw.optional.neon_fp16");
    return cached;
}

int hv_host_has_fhm(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) CAP(fhm, probe_fhm, "hw.optional.arm.FEAT_FHM");
    return cached;
}

int hv_host_has_dotprod(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) CAP(dotprod, probe_dotprod, "hw.optional.arm.FEAT_DotProd");
    return cached;
}

int hv_host_has_sha3(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) CAP(sha3, probe_sha3, "hw.optional.arm.FEAT_SHA3");
    return cached;
}

int hv_host_has_sha512(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) CAP(sha512, probe_sha512, "hw.optional.arm.FEAT_SHA512");
    return cached;
}

int hv_host_has_fcma(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) CAP(fcma, probe_fcma, "hw.optional.arm.FEAT_FCMA");
    return cached;
}

int hv_host_has_jscvt(void) {
    static int cached = CAP_UNKNOWN;
    if (cached == CAP_UNKNOWN) CAP(jscvt, probe_jscvt, "hw.optional.arm.FEAT_JSCVT");
    return cached;
}

void hv_host_features_resolve(void) {
    (void)hv_host_has_lse();
    (void)hv_host_has_lrcpc2();
    (void)hv_host_has_crc32();
    (void)hv_host_has_rdm();
    (void)hv_host_has_fp16();
    (void)hv_host_has_fhm();
    (void)hv_host_has_dotprod();
    (void)hv_host_has_sha3();
    (void)hv_host_has_sha512();
    (void)hv_host_has_fcma();
    (void)hv_host_has_jscvt();
}

const char *hv_host_feature_summary(void) {
    static char buf[320];
    if (buf[0] == '\0') {
        snprintf(buf, sizeof buf,
                 "LSE=%s LRCPC2=%s CRC32=%s RDM=%s FP16=%s FHM=%s DotProd=%s SHA3=%s SHA512=%s FCMA=%s JSCVT=%s%s",
                 hv_host_has_lse() ? "yes" : "no",
                 hv_host_has_lrcpc2() ? "yes" : "no",
                 hv_host_has_crc32() ? "yes" : "no",
                 hv_host_has_rdm() ? "yes" : "no",
                 hv_host_has_fp16() ? "yes" : "no",
                 hv_host_has_fhm() ? "yes" : "no",
                 hv_host_has_dotprod() ? "yes" : "no",
                 hv_host_has_sha3() ? "yes" : "no",
                 hv_host_has_sha512() ? "yes" : "no",
                 hv_host_has_fcma() ? "yes" : "no",
                 hv_host_has_jscvt() ? "yes" : "no",
                 used_probe ? " (probed)" : "");
    }
    return buf;
}
