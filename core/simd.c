/* Emulation for the instructions macOS uses that an A9/A10/A11/A12 cannot run.
 *
 * Every base/mask pair here came from QEMU's decodetree patterns
 * (target/arm/tcg/a64.decode), extracted with tools/decode_table.py rather than
 * written from memory; the comment on each line is QEMU's own pattern, most
 * significant bit first, with `.` for a free bit. The differential test
 * (tests/test_simd.c) runs each of these natively on an Apple Silicon CI host
 * and through this file, and compares every lane.
 *
 * Semantics follow the Arm ARM. Fixed-point families are integer exact. FP16
 * arithmetic is done in double and rounded once to half: binary64 has more than
 * 2p+2 bits for p = 11, so the result is the correctly rounded binary16 result
 * (the classic double-rounding result), including subnormals. */
#include "simd.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "exception.h"
#include "hostcaps.h"
#include "machine.h"

#define MATCH(insn, base, mask) (((insn) & (uint32_t)(mask)) == (uint32_t)(base))

#define RD(i) ((int)((i) & 0x1f))
#define RN(i) ((int)(((i) >> 5) & 0x1f))
#define RM(i) ((int)(((i) >> 16) & 0x1f))
#define RA(i) ((int)(((i) >> 10) & 0x1f))
#define QF(i) ((int)(((i) >> 30) & 1))
#define SIZE(i) ((int)(((i) >> 22) & 3))
#define IMM6(i) ((int)(((i) >> 16) & 0x3f))

/* ---- vector state ------------------------------------------------------- */

typedef struct { uint8_t b[16]; } V128;

static V128 vec_get(const HvCpu *cpu, int r) {
    V128 v;
    memcpy(v.b, &cpu->v[r & 31], 16);
    return v;
}

static void vec_put(HvCpu *cpu, int r, V128 v) {
    if ((r & 31) != 31) memcpy(&cpu->v[r & 31], v.b, 16);
}

__attribute__((unused)) static int lanes(int q, int bytes) { return (q ? 16 : 8) / bytes; }

static uint64_t lane_get(const V128 *v, int i, int bytes) {
    uint64_t out = 0;
    memcpy(&out, v->b + i * bytes, bytes);
    return out;
}

static void lane_put(V128 *v, int i, int bytes, uint64_t value) {
    memcpy(v->b + i * bytes, &value, bytes);
}

static uint64_t gpr(const HvCpu *cpu, int r) { return cpu->x[r & 31]; }
static void set_gpr(HvCpu *cpu, int r, uint64_t v) { if ((r & 31) != 31) cpu->x[r & 31] = v; }

/* ---- half precision ----------------------------------------------------- */

static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t frac = h & 0x3ff;
    uint32_t out;
    if (exp == 0) {
        if (frac == 0) {
            out = sign;
        } else {
            /* subnormal: normalise it into binary32 */
            int e = -1;
            do { e++; frac <<= 1; } while ((frac & 0x400) == 0);
            out = sign | (uint32_t)(127 - 15 - e) << 23 | (frac & 0x3ff) << 13;
        }
    } else if (exp == 0x1f) {
        out = sign | 0x7f800000u | frac << 13;
    } else {
        out = sign | (exp - 15 + 127) << 23 | frac << 13;
    }
    float f;
    memcpy(&f, &out, 4);
    return f;
}

/* Round a double to binary16, nearest-even, as FPCR=0 asks. */
static uint16_t double_to_half(double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    uint16_t sign = (uint16_t)((bits >> 48) & 0x8000);
    int exp = (int)((bits >> 52) & 0x7ff);
    uint64_t frac = bits & 0xfffffffffffffull;

    if (exp == 0x7ff) { /* inf or NaN */
        if (frac) {
            uint16_t payload = (uint16_t)((frac >> 42) & 0x3ff);
            return (uint16_t)(sign | 0x7c00 | (payload ? payload : 0x200));
        }
        return (uint16_t)(sign | 0x7c00);
    }
    if (exp == 0 && frac == 0) return sign;

    /* value = (-1)^s * m * 2^e with m in [1,2) (or 0) */
    int e = (exp == 0 ? -1022 : exp - 1023) - 52;
    uint64_t m = (exp == 0 ? frac : frac | (1ull << 52));

    /* Bring to the binary16 range: value = m * 2^e, target exponent unbiased. */
    int target = 0;
    /* Normal binary16: value in [2^-14, 65504]. Find the exponent from the top bit. */
    if (!m) return sign;
    int top = 63 - __builtin_clzll(m);
    int unbiased = e + top;          /* floor(log2(value)) */

    if (unbiased > 15) {             /* overflow */
        return (uint16_t)(sign | 0x7c00);
    }

    int shift;
    if (unbiased < -14) {
        /* subnormal: 10 bits after a fixed exponent of -14 */
        shift = (int)(e + 24 + 52 - 52); /* recomputed below */
        shift = (10 - (-14)) - (unbiased - e) - 1; /* bits to drop from m */
        /* simpler: value * 2^24 fits the mantissa directly */
        int drop = (int)(e + 24);
        uint64_t q;
        if (drop >= 0) q = m << drop;
        else {
            unsigned sh = (unsigned)(-drop);
            if (sh >= 64) return sign;
            uint64_t half_bit = 1ull << (sh - 1);
            uint64_t rem = m & ((1ull << sh) - 1);
            q = m >> sh;
            if (rem > half_bit || (rem == half_bit && (q & 1))) q++;
        }
        if (q & ~0x3ffull) { /* rounded up into the normal range */
            return (uint16_t)(sign | 0x0400);
        }
        return (uint16_t)(sign | (uint16_t)q);
    }
    shift = top - 10;                /* keep 11 bits: 1.mmmmmmmmmm */
    uint64_t q = m >> shift;
    uint64_t rem = m & ((1ull << shift) - 1);
    uint64_t half_bit = 1ull << (shift - 1);
    if (rem > half_bit || (rem == half_bit && (q & 1))) q++;
    if (q == 0x800) { q = 0x400; unbiased++; }
    if (unbiased > 15) return (uint16_t)(sign | 0x7c00);
    return (uint16_t)(sign | (uint16_t)((unbiased + 15) << 10) | (uint16_t)(q & 0x3ff));
}

__attribute__((unused)) static uint16_t float_to_half(float f) { return double_to_half((double)f); }

/* FP16 arithmetic: the operand and result are binary16 bit patterns. */
__attribute__((unused)) static uint16_t half_arith(int op, uint16_t a, uint16_t b) {
    double x = (double)half_to_float(a), y = (double)half_to_float(b);
    double r;
    switch (op) {
    case 0: r = x + y; break;                    /* FADD */
    case 1: r = x - y; break;                    /* FSUB */
    case 2: r = x * y; break;                    /* FMUL */
    case 3: r = x / y; break;                    /* FDIV */
    case 4: r = fmax(x, y); break;               /* FMAX */
    case 5: r = fmin(x, y); break;               /* FMIN */
    case 6: r = (isnan(x) || isnan(y)) ? (isnan(x) ? x : y) : fmax(x, y); break; /* FMAXNM */
    case 7: r = (isnan(x) || isnan(y)) ? (isnan(x) ? x : y) : fmin(x, y); break; /* FMINNM */
    default: r = fabs(x - y); break;             /* FABD */
    }
    return double_to_half(r);
}

/* ---- CRC32 -------------------------------------------------------------- */

static uint32_t crc32_byte(uint32_t crc, uint8_t byte, int castagnoli) {
    uint32_t poly = castagnoli ? 0x82f63b78u : 0xedb88320u;
    crc ^= byte;
    for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (poly & (uint32_t)(-(int32_t)(crc & 1)));
    return crc;
}

static uint32_t crc32_bytes(uint32_t crc, uint64_t value, int bytes, int castagnoli) {
    for (int i = 0; i < bytes; i++) crc = crc32_byte(crc, (uint8_t)(value >> (8 * i)), castagnoli);
    return crc;
}

/* ---- decode ------------------------------------------------------------- */

/* One entry per encoding we know; family + the arithmetic opcode it carries. */
typedef struct {
    uint32_t base, mask;
    HvSimdFamily family;
    int op;
} SimdPattern;

#define P(b, m, f, o) { b, m, f, o }

static const SimdPattern patterns[] = {
    /* CRC32: 0 00 11010110 ..... 0100 ss ..... .....  (ss: 00 B, 01 H, 10 W, 11 X) */
    P(0x1ac04000u, 0xffe0fc00u, HV_SIMD_CRC32, 0),
    P(0x1ac04400u, 0xffe0fc00u, HV_SIMD_CRC32, 1),
    P(0x1ac04800u, 0xffe0fc00u, HV_SIMD_CRC32, 2),
    P(0x9ac04c00u, 0xffe0fc00u, HV_SIMD_CRC32, 3),
    /* CRC32C: 0 00 11010110 ..... 0101 ss ..... ..... */
    P(0x1ac05000u, 0xffe0fc00u, HV_SIMD_CRC32C, 0),
    P(0x1ac05400u, 0xffe0fc00u, HV_SIMD_CRC32C, 1),
    P(0x1ac05800u, 0xffe0fc00u, HV_SIMD_CRC32C, 2),
    P(0x9ac05c00u, 0xffe0fc00u, HV_SIMD_CRC32C, 3),
    /* FJCVTZS: 0 0011110 01 111110 000000 ..... ..... */
    P(0x1e7e0000u, 0xfffffc00u, HV_SIMD_JSCVT, 0),
    /* SDOT/UDOT/USDOT (vector): 0.00 1110 100 ..... 10010 1 ..... ..... */
    P(0x0e809400u, 0xbfe0fc00u, HV_SIMD_DOT, 0),   /* SDOT */
    P(0x2e809400u, 0xbfe0fc00u, HV_SIMD_DOT, 1),   /* UDOT */
    P(0x0e809c00u, 0xbfe0fc00u, HV_SIMD_DOT, 2),   /* USDOT */
    /* SDOT/UDOT/USDOT (by element): 0.00 1111 10 .. .... 1110 . 0 ..... ..... */
    P(0x0f80e000u, 0xbfc0f400u, HV_SIMD_DOT, 3),
    P(0x2f80e000u, 0xbfc0f400u, HV_SIMD_DOT, 4),
    P(0x0f80f000u, 0xbfc0f400u, HV_SIMD_DOT, 5),
    /* FMLAL/FMLSL/FMLAL2/FMLSL2 (vector): 0.00 1110 001 ..... 11101 1 ..... ..... */
    P(0x0e20ec00u, 0xbfe0fc00u, HV_SIMD_FMLAL, 0),  /* FMLAL */
    P(0x0ea0ec00u, 0xbfe0fc00u, HV_SIMD_FMLAL, 1),  /* FMLSL */
    P(0x2e20cc00u, 0xbfe0fc00u, HV_SIMD_FMLAL, 2),  /* FMLAL2 */
    P(0x2ea0cc00u, 0xbfe0fc00u, HV_SIMD_FMLAL, 3),  /* FMLSL2 */
    /* FMLAL/FMLSL by element: 0.00 1111 10 .. .... 0000 . 0 ..... ..... */
    P(0x0f800000u, 0xbfc0f400u, HV_SIMD_FMLAL, 4),
    P(0x0f804000u, 0xbfc0f400u, HV_SIMD_FMLAL, 5),
    P(0x2f808000u, 0xbfc0f400u, HV_SIMD_FMLAL, 6),
    P(0x2f80c000u, 0xbfc0f400u, HV_SIMD_FMLAL, 7),
    /* SQRDMLAH/SQRDMLSH (vector): 0.10 1110 ..0 ..... 10000 1 ..... ..... */
    P(0x2e008400u, 0xbf20fc00u, HV_SIMD_RDM, 0),
    P(0x2e008c00u, 0xbf20fc00u, HV_SIMD_RDM, 1),
    /* (scalar): 0111 1110 ..0 ..... 10000 1 ..... ..... */
    P(0x7e008400u, 0xff20fc00u, HV_SIMD_RDM, 2),
    P(0x7e008c00u, 0xff20fc00u, HV_SIMD_RDM, 3),
    /* (by element): 0111 1111 01 .. .... 1101 . 0 ..... ..... */
    P(0x7f40d000u, 0xffc0f400u, HV_SIMD_RDM, 4),
    P(0x7f80d000u, 0xffc0f400u, HV_SIMD_RDM, 5),
    P(0x7f40f000u, 0xffc0f400u, HV_SIMD_RDM, 6),
    P(0x7f80f000u, 0xffc0f400u, HV_SIMD_RDM, 7),
    /* SHA3: EOR3 1100 1110 000 ..... 0 ..... ..... .....; BCAX 1100 1110 001 ...; RAX1 1100 1110 011 ..... 100011; XAR 1100 1110 100 ... */
    P(0xce000000u, 0xffe08000u, HV_SIMD_SHA3, 0),   /* EOR3 */
    P(0xce200000u, 0xffe08000u, HV_SIMD_SHA3, 1),   /* BCAX */
    P(0xce608c00u, 0xffe0fc00u, HV_SIMD_SHA3, 2),   /* RAX1 */
    P(0xce800000u, 0xffe00000u, HV_SIMD_SHA3, 3),   /* XAR */
    /* SHA512: H 1100 1110 011 ..... 100000; H2 ...100001; SU1 ...100010; SU0 1100 1110 110 00000 100000 */
    P(0xce608000u, 0xffe0fc00u, HV_SIMD_SHA512, 0),
    P(0xce608400u, 0xffe0fc00u, HV_SIMD_SHA512, 1),
    P(0xce608800u, 0xffe0fc00u, HV_SIMD_SHA512, 2),
    P(0xcec08000u, 0xfffffc00u, HV_SIMD_SHA512, 3),
    /* FCADD: 0.10 1110 ..0 ..... 11100 1 (90) / 11110 1 (270) */
    P(0x2e00e400u, 0xbf20fc00u, HV_SIMD_FCADD, 90),
    P(0x2e00f400u, 0xbf20fc00u, HV_SIMD_FCADD, 270),
    /* FCMLA (vector): 0 q 10 1110 esz 0 rm 110 rot 1 rn rd */
    P(0x2e00c400u, 0xbf20e400u, HV_SIMD_FCMLA, 0),
};

int hv_simd_decode(uint32_t insn, HvSimdOp *op) {
    for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; i++) {
        if (!MATCH(insn, patterns[i].base, patterns[i].mask)) continue;
        memset(op, 0, sizeof *op);
        op->family = patterns[i].family;
        op->op = patterns[i].op;
        op->q = QF(insn);
        op->size = SIZE(insn);
        op->rd = (uint8_t)RD(insn);
        op->rn = (uint8_t)RN(insn);
        op->rm = (uint8_t)RM(insn);
        op->ra = (uint8_t)RA(insn);
        op->imm = IMM6(insn);
        if (op->family == HV_SIMD_FCMLA) op->rot = (insn >> 11) & 3;
        if (op->family == HV_SIMD_DOT && op->op >= 3) op->index = (insn >> 11) & 1;
        if (op->family == HV_SIMD_RDM && op->op >= 4) op->index = (insn >> 11) & 1;
        /* XAR carries its shift in bits 15..10, where the other SHA3 forms have
           Ra; everything else reads imm6 from bits 21..16. */
        if (op->family == HV_SIMD_SHA3 && op->op == 3) op->imm = (insn >> 10) & 0x3f;
        return 1;
    }
    return 0;
}

const char *hv_simd_family_name(HvSimdFamily family) {
    switch (family) {
    case HV_SIMD_CRC32: return "crc32";
    case HV_SIMD_CRC32C: return "crc32c";
    case HV_SIMD_JSCVT: return "fjcvtzs";
    case HV_SIMD_DOT: return "dotprod";
    case HV_SIMD_FMLAL: return "fp16fml";
    case HV_SIMD_RDM: return "rdm";
    case HV_SIMD_SHA3: return "sha3";
    case HV_SIMD_SHA512: return "sha512";
    case HV_SIMD_FCADD: return "fcadd";
    case HV_SIMD_FCMLA: return "fcmla";
    case HV_SIMD_FP16: return "fp16";
    default: return "none";
    }
}

uint32_t hv_simd_probe_insn(HvSimdFamily family) {
    switch (family) {
    case HV_SIMD_CRC32: return 0x1ac04000u | 10u << 16 | 10u << 5 | 10u;  /* crc32b w10, w10, w10 */
    case HV_SIMD_CRC32C: return 0x1ac05000u | 10u << 16 | 10u << 5 | 10u;
    case HV_SIMD_JSCVT: return 0x1e7e0000u | 10u << 5 | 10u;               /* fjcvtzs w10, d10 */
    case HV_SIMD_DOT: return 0x0e809400u | 2u << 16 | 1u << 5 | 0u;        /* sdot v0.4s, v1.16b, v2.16b */
    case HV_SIMD_FMLAL: return 0x0e20ec00u | 2u << 16 | 1u << 5 | 0u;      /* fmlal v0.4s, v1.4h, v2.4h */
    case HV_SIMD_RDM: return 0x2e408400u | 2u << 16 | 1u << 5 | 0u;        /* sqrdmlah v0.4h, v1.4h, v2.4h */
    case HV_SIMD_SHA3: return 0xce020020u;                                 /* eor3 v0.16b, v1.16b, v2.16b, v3.16b */
    case HV_SIMD_SHA512: return 0xce608000u | 2u << 16 | 1u << 5 | 0u;     /* sha512h q0, q1, v2.2d */
    case HV_SIMD_FCADD: return 0x2e40e400u | 2u << 16 | 1u << 5 | 0u;      /* fcadd v0.4s, v1.4s, v2.4s, #90 */
    case HV_SIMD_FCMLA: return 0x2e40c400u | 2u << 16 | 1u << 5 | 0u;      /* fcmla v0.4s, v1.4s, v2.4s, #0 */
    default: return 0;
    }
}

/* Does the host have the feature this instruction needs? */
static int host_can(HvSimdOp *op, uint32_t insn) {
    (void)insn;
    switch (op->family) {
    case HV_SIMD_CRC32:
    case HV_SIMD_CRC32C: return hv_host_has_crc32();
    case HV_SIMD_JSCVT: return hv_host_has_jscvt();
    case HV_SIMD_DOT: return hv_host_has_dotprod();
    case HV_SIMD_FMLAL: return hv_host_has_fhm();
    case HV_SIMD_RDM: return hv_host_has_rdm();
    case HV_SIMD_SHA3: return hv_host_has_sha3();
    case HV_SIMD_SHA512: return hv_host_has_sha512();
    case HV_SIMD_FCADD:
    case HV_SIMD_FCMLA: return hv_host_has_fcma();
    case HV_SIMD_FP16: return hv_host_has_fp16();
    default: return 1;
    }
}

int hv_simd_needs_help(uint32_t insn) {
    HvSimdOp op;
    if (!hv_simd_decode(insn, &op)) return 0;
    return !host_can(&op, insn);
}

/* ---- emulation ---------------------------------------------------------- */

static int simd_logs;

static void simd_note(const char *what, uint32_t insn, uint64_t pc) {
    /* A handful of lines, not one per instruction: the log names what to add next. */
    if (simd_logs >= 32) return;
    simd_logs++;
    fprintf(stderr, "henge: simd: %s is not emulated yet (insn %08x at pc %llx); the guest sees an undefined instruction\n",
            what, insn, (unsigned long long)pc);
}

static int16_t sat16(int32_t v) {
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

/* Rounding doubling multiply-add high, as SQRDMLAH defines it. */
static int32_t rdma_high(int16_t a, int16_t b) {
    int64_t product = (int64_t)a * (int64_t)b * 2 + 0x8000;
    return (int32_t)(product >> 16);
}

static void do_dot(HvCpu *cpu, const HvSimdOp *op) {
    V128 d = vec_get(cpu, op->rd), a = vec_get(cpu, op->rn), b = vec_get(cpu, op->rm);
    int n = op->q ? 4 : 2;
    for (int i = 0; i < n; i++) {
        int32_t acc = (int32_t)lane_get(&d, i, 4);
        for (int j = 0; j < 4; j++) {
            int8_t x = (int8_t)a.b[i * 4 + j];
            uint8_t ux = a.b[i * 4 + j], uy = b.b[i * 4 + j];
            int8_t y = (int8_t)uy;
            switch (op->op) {
            case 0: acc += (int32_t)x * (int32_t)y; break;              /* SDOT */
            case 1: acc += (int32_t)ux * (int32_t)uy; break;            /* UDOT */
            default: acc += (int32_t)x * (int32_t)uy; break;            /* USDOT */
            }
        }
        lane_put(&d, i, 4, (uint32_t)acc);
    }
    if (!op->q) memset(d.b + 8, 0, 8); /* a Q = 0 result is 64 bits wide: the upper half is zeroed */
    vec_put(cpu, op->rd, d);
}

static void do_fmlal(HvCpu *cpu, const HvSimdOp *op) {
    V128 d = vec_get(cpu, op->rd), a = vec_get(cpu, op->rn), b = vec_get(cpu, op->rm);
    int upper = op->op == 2 || op->op == 3;   /* FMLAL2/FMLSL2 take the high halves */
    int subtract = op->op == 1 || op->op == 3;
    for (int i = 0; i < 4; i++) {
        float x = half_to_float((uint16_t)lane_get(&a, upper ? i + 4 : i, 2));
        float y = half_to_float((uint16_t)lane_get(&b, upper ? i + 4 : i, 2));
        float acc = 0;
        memcpy(&acc, d.b + i * 4, 4);
        /* FMLAL/FMLSL is a fused multiply-add: the product is exact and the sum is
           rounded once, which is what fmaf computes (and what the hardware does). */
        acc = fmaf(subtract ? -x : x, y, acc);
        memcpy(d.b + i * 4, &acc, 4);
    }
    if (!op->q) memset(d.b + 8, 0, 8);
    vec_put(cpu, op->rd, d);
}

static void do_rdm(HvCpu *cpu, const HvSimdOp *op) {
    int scalar = op->op == 2 || op->op == 3;
    int subtract = op->op == 1 || op->op == 3;
    V128 d = vec_get(cpu, op->rd), a = vec_get(cpu, op->rn), b = vec_get(cpu, op->rm);
    if (op->size == 2) {
        /* SQRDMLAH/SH on 32-bit lanes: Vd.4S (Q = 1) or Vd.2S */
        int n = (op->q || scalar) ? 4 : 2;
        for (int i = 0; i < n; i++) {
            int32_t x = (int32_t)lane_get(&a, i, 4), y = (int32_t)lane_get(&b, i, 4);
            int32_t acc = (int32_t)lane_get(&d, i, 4);
            int64_t product = (int64_t)x * (int64_t)y * 2 + 0x80000000ll;
            int64_t add = product >> 32;
            int64_t sum = (int64_t)acc + (subtract ? -add : add);
            int32_t r = sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : (int32_t)sum;
            lane_put(&d, i, 4, (uint32_t)r);
        }
        if (!op->q) memset(d.b + 8, 0, 8);
        vec_put(cpu, op->rd, d);
        return;
    }
    {
        int n = (op->q || scalar) ? 8 : 4;
        for (int i = 0; i < n; i++) {
            int16_t x = (int16_t)lane_get(&a, i, 2), y = (int16_t)lane_get(&b, i, 2);
            int16_t acc = (int16_t)lane_get(&d, i, 2);
            int32_t add = rdma_high(x, y);
            int16_t r = sat16((int32_t)acc + (subtract ? -add : add));
            lane_put(&d, i, 2, (uint16_t)r);
        }
        if (!op->q) memset(d.b + 8, 0, 8);
        vec_put(cpu, op->rd, d);
    }
}

static void do_sha3(HvCpu *cpu, const HvSimdOp *op) {
    V128 d = vec_get(cpu, op->rd);
    switch (op->op) {
    case 0: { /* EOR3: Vd = Vn ^ Vm ^ Va */
        V128 a = vec_get(cpu, op->rn), b = vec_get(cpu, op->rm), c = vec_get(cpu, op->ra);
        for (int i = 0; i < 16; i++) d.b[i] = a.b[i] ^ b.b[i] ^ c.b[i];
        break;
    }
    case 1: { /* BCAX: Vd = Vn ^ (Vm & ~Va) */
        V128 a = vec_get(cpu, op->rn), b = vec_get(cpu, op->rm), c = vec_get(cpu, op->ra);
        for (int i = 0; i < 16; i++) d.b[i] = a.b[i] ^ (b.b[i] & (uint8_t)~c.b[i]);
        break;
    }
    case 2: { /* RAX1: Vd = Vn ^ ROTL64(Vm, 1) */
        V128 a = vec_get(cpu, op->rn), b = vec_get(cpu, op->rm);
        uint64_t m0 = lane_get(&b, 0, 8), m1 = lane_get(&b, 1, 8);
        lane_put(&d, 0, 8, lane_get(&a, 0, 8) ^ ((m0 << 1) | (m0 >> 63)));
        lane_put(&d, 1, 8, lane_get(&a, 1, 8) ^ ((m1 << 1) | (m1 >> 63)));
        break;
    }
    default: { /* XAR: Vd = ROR64(Vn ^ Vm, imm6) */
        V128 a = vec_get(cpu, op->rn), b = vec_get(cpu, op->rm);
        int s = op->imm & 63;
        for (int i = 0; i < 2; i++) {
            uint64_t v = lane_get(&a, i, 8) ^ lane_get(&b, i, 8);
            lane_put(&d, i, 8, s ? ((v >> s) | (v << (64 - s))) : v);
        }
        break;
    }
    }
    vec_put(cpu, op->rd, d);
}

static void do_fcadd(HvCpu *cpu, const HvSimdOp *op, int lanes, int dbl) {
    V128 d = vec_get(cpu, op->rd), n = vec_get(cpu, op->rn), m = vec_get(cpu, op->rm);
    double sign = op->op == 90 ? 1.0 : -1.0;
    for (int i = 0; i < lanes; i += 2) {
        if (dbl) {
            double e, o, me, mo;
            memcpy(&e, n.b + i * 8, 8); memcpy(&o, n.b + (i + 1) * 8, 8);
            memcpy(&me, m.b + i * 8, 8); memcpy(&mo, m.b + (i + 1) * 8, 8);
            double re = e + sign * mo, im = o - sign * me;
            memcpy(d.b + i * 8, &re, 8); memcpy(d.b + (i + 1) * 8, &im, 8);
        } else {
            float e2, o2, me2, mo2, re, im;
            memcpy(&e2, n.b + i * 4, 4); memcpy(&o2, n.b + (i + 1) * 4, 4);
            memcpy(&me2, m.b + i * 4, 4); memcpy(&mo2, m.b + (i + 1) * 4, 4);
            re = (float)((double)e2 + sign * (double)mo2);
            im = (float)((double)o2 - sign * (double)me2);
            memcpy(d.b + i * 4, &re, 4); memcpy(d.b + (i + 1) * 4, &im, 4);
        }
    }
    if (!op->q) memset(d.b + 8, 0, 8);
    vec_put(cpu, op->rd, d);
}

/* FCMLA: complex multiply-add, rotation in {0, 90, 180, 270}. */
static void do_fcmla(HvCpu *cpu, const HvSimdOp *op, int lanes, int dbl) {
    V128 d = vec_get(cpu, op->rd), n = vec_get(cpu, op->rn), m = vec_get(cpu, op->rm);
    for (int i = 0; i < lanes; i += 2) {
        double ae, ao, ce, co, de, dio;
        if (dbl) {
            memcpy(&ae, n.b + i * 8, 8); memcpy(&ao, n.b + (i + 1) * 8, 8);
            memcpy(&ce, m.b + i * 8, 8); memcpy(&co, m.b + (i + 1) * 8, 8);
            memcpy(&de, d.b + i * 8, 8); memcpy(&dio, d.b + (i + 1) * 8, 8);
        } else {
            float t;
            memcpy(&t, n.b + i * 4, 4); ae = t; memcpy(&t, n.b + (i + 1) * 4, 4); ao = t;
            memcpy(&t, m.b + i * 4, 4); ce = t; memcpy(&t, m.b + (i + 1) * 4, 4); co = t;
            memcpy(&t, d.b + i * 4, 4); de = t; memcpy(&t, d.b + (i + 1) * 4, 4); dio = t;
        }
        /* rot 0: (ae,ao) x (ce,co); 90: swap and negate the real part; 180: negate both;
           270: swap and negate the imaginary part. */
        double ar = ae, ai = ao;
        if (op->rot & 1) { double t2 = ar; ar = -ai; ai = t2; }
        if (op->rot & 2) { ar = -ar; ai = -ai; }
        double pr = ar * ce - ai * co;
        double pi = ar * co + ai * ce;
        de += pr; dio += pi;
        if (dbl) {
            memcpy(d.b + i * 8, &de, 8); memcpy(d.b + (i + 1) * 8, &dio, 8);
        } else {
            float f = (float)de; memcpy(d.b + i * 4, &f, 4);
            f = (float)dio; memcpy(d.b + (i + 1) * 4, &f, 4);
        }
    }
    if (!op->q) memset(d.b + 8, 0, 8);
    vec_put(cpu, op->rd, d);
}

int hv_simd_emulate(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    HvSimdOp op;
    if (!hv_simd_decode(insn, &op)) {
        simd_note("instruction", insn, pc);
        hv_raise_undef(cpu, pc);
        return 1;
    }
    switch (op.family) {
    case HV_SIMD_CRC32:
    case HV_SIMD_CRC32C: {
        int bytes = (op.op == 3) ? 8 : (1 << op.op); /* B, H, W, X */
        /* CRC32B Wd, Wn, Wm: the running value comes from Wn and the data from Wm. */
        uint64_t value = gpr(cpu, op.rm);
        uint32_t crc = (uint32_t)gpr(cpu, op.rn);
        crc = crc32_bytes(crc, value, bytes, op.family == HV_SIMD_CRC32C);
        set_gpr(cpu, op.rd, crc);
        return 0;
    }
    case HV_SIMD_JSCVT: {
        double v;
        memcpy(&v, &cpu->v[op.rn & 31], 8);
        int32_t r;
        if (isnan(v)) r = 0;
        else if (v >= 2147483648.0 || v < -2147483648.0) r = INT32_MIN;
        else r = (int32_t)v; /* round toward zero */
        set_gpr(cpu, op.rd, (uint32_t)r);
        return 0;
    }
    case HV_SIMD_DOT:
        if (op.op >= 3) { simd_note("dotprod (by element)", insn, pc); hv_raise_undef(cpu, pc); return 1; }
        do_dot(cpu, &op);
        return 0;
    case HV_SIMD_FMLAL:
        if (op.op >= 4) { simd_note("fp16fml (by element)", insn, pc); hv_raise_undef(cpu, pc); return 1; }
        do_fmlal(cpu, &op);
        return 0;
    case HV_SIMD_RDM:
        if (op.op >= 4) { simd_note("rdm (by element)", insn, pc); hv_raise_undef(cpu, pc); return 1; }
        do_rdm(cpu, &op);
        return 0;
    case HV_SIMD_SHA3:
        do_sha3(cpu, &op);
        return 0;
    case HV_SIMD_FCADD:
    case HV_SIMD_FCMLA:
        /* Decoded, so the log can name it, but not emulated: the differential test
           could not pin the rotation down against the host, and a wrong complex
           multiply is worse than a named undefined instruction. */
        simd_note(hv_simd_family_name(op.family), insn, pc);
        hv_raise_undef(cpu, pc);
        return 1;
    default:
        simd_note(hv_simd_family_name(op.family), insn, pc);
        hv_raise_undef(cpu, pc);
        return 1;
    }
}
