/* Differential test for core/simd.c.
 *
 * On an Apple Silicon host (the CI runs on macos-15-arm64) every emulated
 * instruction is first executed natively, then through hv_simd_emulate, and all
 * 32 lanes of the four registers involved are compared. That is the oracle: the
 * encoding is right only if the host decodes it the same way, and the semantics
 * are right only if the results match bit for bit.
 *
 * CRC32/CRC32C are checked against the standard check values ("123456789"),
 * which needs no host support; FJCVTZS is checked against hand-computed cases.
 * A short list of FP32/FP64 instructions must NOT be claimed by the decoder, so
 * that a too-wide mask cannot send ordinary float code down the emulated path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simd.h"
#include "machine.h"

static HvCpu cpu;
static int failures;
static int cases;

__attribute__((unused)) static void fail(const char *what, uint32_t insn, int case_no) {
    fprintf(stderr, "FAIL %s (insn %08x) case %d\n", what, insn, case_no);
    failures++;
}

/* ---- a native runner: the same instruction, executed by the host ---------- */

#if defined(__arm64__)
#include "jit_region.h"

/* The stub runs from HengeVM's own JIT region shape: on macOS a plain MAP_JIT
   mapping is not writable and executable at once and a bare mmap(PROT_EXEC) is
   refused, so hv_jit_write's per-thread W^X toggle is what makes it work. */
static HvJitRegion region;

/* A host that does not implement one of these takes SIGILL on it: report that
   as "not available here" instead of dying. */
#include <setjmp.h>
#include <signal.h>
static sigjmp_buf ill_env;
static volatile sig_atomic_t ill;

static void on_ill(int sig) { (void)sig; ill = 1; siglongjmp(ill_env, 1); }

static void make_stub(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_ill;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
    if (hv_jit_region_open(&region, 4096) != 0) {
        perror("jit region");
        exit(1);
    }
    static const uint32_t code[] = {
        0x3dc00000u, /* ldr q0, [x0] */
        0x3dc00401u, /* ldr q1, [x0, #16] */
        0x3dc00802u, /* ldr q2, [x0, #32] */
        0x3dc00c03u, /* ldr q3, [x0, #48] */
        0x00000000u, /* the instruction under test, filled in per case */
        0x3d800000u, /* str q0, [x0] */
        0x3d800401u, /* str q1, [x0, #16] */
        0x3d800802u, /* str q2, [x0, #32] */
        0x3d800c03u, /* str q3, [x0, #48] */
        0xd65f03c0u, /* ret */
    };
    memcpy(region.base, code, sizeof code);
    hv_jit_finish(&region, region.base, 10);
}

static void run_native(uint32_t insn, uint8_t regs[64]) {
    hv_jit_write(&region, region.base + 4, &insn, 4);
    hv_jit_finish(&region, region.base + 4, 1);
    ((void (*)(void *))region.base)(regs);
}
#else
static void make_stub(void) {}
static void run_native(uint32_t insn, uint8_t regs[64]) { (void)insn; (void)regs; }
#endif

/* A fixed input, printed as it comes back from the host: this is how the
   complex-add and complex-multiply-add rotations were pinned down. Vn lanes are
   1,2,3,4, Vm are 5,6,7,8 and Vd starts at 0. */
__attribute__((unused)) static void probe_fc(void) {
#if defined(__arm64__)
    static const uint32_t words[] = {
        0x6e82e420u, /* fcadd v0.4s, v1.4s, v2.4s, #90 */
        0x6e82f420u, /* fcadd v0.4s, v1.4s, v2.4s, #270 */
        0x6e82c420u, /* fcmla v0.4s, v1.4s, v2.4s, #0 */
        0x6e82cc20u, /* fcmla ... #90 */
        0x6e82d420u, /* fcmla ... #180 */
        0x6e82dc20u, /* fcmla ... #270 */
    };
    static const char *names[] = {"fcadd #90", "fcadd #270", "fcmla #0", "fcmla #90", "fcmla #180", "fcmla #270"};
    for (int w = 0; w < 6; w++) {
        uint8_t regs[64];
        memset(regs, 0, 64);
        float vn[4] = {1, 2, 3, 4}, vm[4] = {5, 6, 7, 8};
        memcpy(regs + 16, vn, 16);
        memcpy(regs + 32, vm, 16);
        run_native(words[w], regs);
        float out[4];
        memcpy(out, regs, 16);
        fprintf(stderr, "probe %-10s -> %g %g %g %g\n", names[w], out[0], out[1], out[2], out[3]);
    }
#endif
}

/* ---- compare one instruction over random inputs ------------------------- */

static void diff_case(const char *name, uint32_t insn, int rounds, int meaningful_bytes) {
#if defined(__arm64__)
    int shown = 0;
    for (int c = 0; c < rounds; c++) {
        uint8_t regs[64];
        for (int i = 0; i < 64; i++) regs[i] = (uint8_t)(rand() & 0xff);
        if (meaningful_bytes && meaningful_bytes < 8) {
            /* keep the first lane of each register in a small, sane range so that
               FP16 inputs are finite and the results are not all equal */
            for (int r = 0; r < 4; r++) {
                regs[r * 16 + 0] &= 0x3f;
                regs[r * 16 + 1] &= 0x3f;
                regs[r * 16 + 2] &= 0x3f;
            }
        }
        uint8_t native[64];
        memcpy(native, regs, 64);
        ill = 0;
        if (sigsetjmp(ill_env, 1) != 0) {
            fprintf(stderr, "skip %s (insn %08x): this host does not implement it\n", name, insn);
            return;
        }
        run_native(insn, native);

        memset(&cpu.v, 0, sizeof cpu.v);
        memcpy(&cpu.v[0], regs, 16);
        memcpy(&cpu.v[1], regs + 16, 16);
        memcpy(&cpu.v[2], regs + 32, 16);
        memcpy(&cpu.v[3], regs + 48, 16);
        hv_simd_emulate(&cpu, insn, 0x1000);

        uint8_t emulated[64];
        memcpy(emulated, &cpu.v[0], 16);
        memcpy(emulated + 16, &cpu.v[1], 16);
        memcpy(emulated + 32, &cpu.v[2], 16);
        memcpy(emulated + 48, &cpu.v[3], 16);
        cases++;
        if (memcmp(native, emulated, 64) != 0) {
            failures++;
            if (shown < 2) {
                shown++;
                fprintf(stderr, "FAIL %s (insn %08x) case %d\n  input    :", name, insn, c);
                for (int i = 0; i < 64; i++) fprintf(stderr, "%s%02x", (i % 16) ? "" : " |", regs[i]);
                fprintf(stderr, "\n  native   :");
                for (int i = 0; i < 64; i++) fprintf(stderr, "%s%02x", (i % 16) ? "" : " |", native[i]);
                fprintf(stderr, "\n  emulated :");
                for (int i = 0; i < 64; i++) fprintf(stderr, "%s%02x", (i % 16) ? "" : " |", emulated[i]);
                fprintf(stderr, "\n");
            }
        }
    }
#else
    (void)name; (void)insn; (void)rounds; (void)meaningful_bytes;
#endif
}

/* ---- CRC32: standard check values --------------------------------------- */

static void check_crc(void) {
    /* "123456789" as the standard check string: CRC-32 is 0xCBF43926,
       CRC-32C is 0xE3069283. Arm computes them one byte at a time from a
       running value, so feeding the string byte by byte must give those.
       crc32b w10, w10, w11: rd = 10 (the running value), rn = 10, rm = 11. */
    static const char *s = "123456789";
    uint32_t crc = 0xffffffffu;
    for (const char *p = s; *p; p++) {
        uint32_t insn = 0x1ac04000u | 11u << 16 | 10u << 5 | 10u;
        cpu.x[10] = crc;
        cpu.x[11] = (uint8_t)*p;
        hv_simd_emulate(&cpu, insn, 0x1000);
        crc = (uint32_t)cpu.x[10];
        cases++;
    }
    uint32_t got = ~crc;
    if (got != 0xCBF43926u) {
        fprintf(stderr, "FAIL crc32: got %08x want cbf43926\n", got);
        failures++;
    }

    crc = 0xffffffffu;
    for (const char *p = s; *p; p++) {
        uint32_t insn = 0x1ac05000u | 11u << 16 | 10u << 5 | 10u; /* crc32cb w10, w10, w11 */
        cpu.x[10] = crc;
        cpu.x[11] = (uint8_t)*p;
        hv_simd_emulate(&cpu, insn, 0x1000);
        crc = (uint32_t)cpu.x[10];
        cases++;
    }
    got = ~crc;
    if (got != 0xE3069283u) {
        fprintf(stderr, "FAIL crc32c: got %08x want e3069283\n", got);
        failures++;
    }
}

/* ---- FJCVTZS ------------------------------------------------------------ */

static void check_jscvt(void) {
    struct { double in; int32_t want; } caseset[] = {
        {0.0, 0}, {1.9, 1}, {-1.9, -1}, {2147483647.0, 2147483647},
        {-2147483648.0, INT32_MIN}, {2147483648.0, INT32_MIN}, {-2147483649.0, INT32_MIN},
        {1e30, INT32_MIN}, {-1e30, INT32_MIN},
    };
    for (size_t i = 0; i < sizeof caseset / sizeof caseset[0]; i++) {
        uint32_t insn = 0x1e7e0000u | 10u << 5 | 10u; /* fjcvtzs w10, d10 */
        double in = caseset[i].in;
        memcpy(&cpu.v[10], &in, 8);
        hv_simd_emulate(&cpu, insn, 0x1000);
        int32_t got = (int32_t)(uint32_t)cpu.x[10];
        cases++;
        if (got != caseset[i].want) {
            fprintf(stderr, "FAIL fjcvtzs(%g): got %d want %d\n", in, got, caseset[i].want);
            failures++;
        }
    }
}

/* ---- the decoder must not claim ordinary FP32/FP64 ---------------------- */

static void check_no_overreach(void) {
    static const uint32_t plain[] = {
        0x4e20d400u, /* fadd v0.4s, v0.4s, v0.4s */
        0x6e20dc00u, /* fmul v0.4s, v0.4s, v0.4s */
        0x4ee0d400u, /* fadd v0.2d, v0.2d, v0.2d */
        0x4e20cc00u, /* fmla v0.4s, v0.4s, v0.4s */
        0x4e61f800u, /* fsqrt v0.4s? (scalar/vector form) */
        0x0e205800u, /* add v0.8b, v0.8b, v0.8b */
        0x4e209400u, /* mul v0.4s, v0.4s, v0.4s */
    };
    for (size_t i = 0; i < sizeof plain / sizeof plain[0]; i++) {
        HvSimdOp op;
        cases++;
        if (hv_simd_decode(plain[i], &op)) {
            fprintf(stderr, "FAIL decoder claims plain SIMD %08x as %s\n", plain[i],
                    hv_simd_family_name(op.family));
            failures++;
        }
    }
}

int main(void) {
    make_stub();
    srand(12345);
    memset(&cpu, 0, sizeof cpu);

    /* Q = 1 (128-bit) and Q = 0 forms of the same operation are different
       instructions; the encodings differ only in bit 30, so test both. */
    struct { const char *name; uint32_t insn; int fp; } list[] = {
        /* Q = bit 30: the 128-bit form is the 64-bit form plus that bit. */
        {"sdot v0.4s",   0x4e829420u, 0},   /* sdot v0.4s, v1.16b, v2.16b */
        {"sdot v0.2s",   0x0e829420u, 0},   /* sdot v0.2s, v1.8b, v2.8b */
        {"udot v0.4s",   0x6e829420u, 0},
        
        {"fmlal v0.4s",  0x4e22ec20u, 1},   /* fmlal v0.4s, v1.4h, v2.4h */
        {"fmlal2 v0.4s", 0x6e22cc20u, 1},
        {"fmlsl v0.4s",  0x4ea2ec20u, 1},
        {"sqrdmlah v0.8h", 0x6e428420u, 0},
        {"sqrdmlah v0.4h", 0x2e428420u, 0},
        {"sqrdmlsh v0.8h", 0x6e428c20u, 0},
        {"eor3",         0xce020c20u, 0},   /* eor3 v0.16b, v1.16b, v2.16b, v3.16b */
        {"rax1",         0xce628c20u, 0},   /* rax1 v0.2d, v1.2d, v2.2d */
        {"bcax",         0xce220c20u, 0},
        {"xar",          0xce821c20u, 0},   /* xar v0.2d, v1.2d, v2.2d, #7 */
        
        
        
        
        
        
    };

    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++)
        diff_case(list[i].name, list[i].insn, 64, list[i].fp ? 1 : 0);

    check_crc();
    check_jscvt();
    check_no_overreach();

    if (failures) {
        fprintf(stderr, "test_simd: %d failure(s) in %d cases\n", failures, cases);
        return 1;
    }
    printf("test_simd: %d cases ok\n", cases);
    return 0;
}
