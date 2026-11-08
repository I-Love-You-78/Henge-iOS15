#ifndef HENGEVM_SIMD_H
#define HENGEVM_SIMD_H

#include <stdint.h>

#include "cpu.h"

/* Instructions the guest may run that arrived after Armv8.0.
 *
 * macOS arm64 code is compiled for apple-m1 by default (clang's Darwin driver
 * picks that CPU for every aarch64 macOS triple), and apple-m1 implies Armv8.5
 * plus FEAT_FullFP16, FP16FML, DotProd, SHA3, LSE, ComplxNum and JSCVT. An A9
 * (v8.0 only) or an A10/A11/A12 executes none or only some of them, so the
 * translator hands each one to this file instead of copying it into a block,
 * and it is emulated lane by lane. See docs/ios15-a9-support.md.
 *
 * Encodings are taken from QEMU's decodetree patterns, not from memory: each
 * mask/base pair below carries its pattern as a comment. */

typedef enum {
    HV_SIMD_NONE = 0,
    HV_SIMD_CRC32,   /* FEAT_CRC32: CRC32B/H/W/X (v8.0 optional, A10 and later) */
    HV_SIMD_CRC32C,  /* FEAT_CRC32: CRC32CB/CH/CW/CX */
    HV_SIMD_JSCVT,   /* FEAT_JSCVT: FJCVTZS (v8.3) */
    HV_SIMD_DOT,     /* FEAT_DotProd: SDOT/UDOT/USDOT (v8.2) */
    HV_SIMD_FMLAL,   /* FEAT_FP16FML: FMLAL/FMLSL/FMLAL2/FMLSL2 (v8.2) */
    HV_SIMD_RDM,     /* FEAT_RDM: SQRDMLAH/SQRDMLSH (v8.1) */
    HV_SIMD_SHA3,    /* FEAT_SHA3: EOR3, RAX1, BCAX, XAR (v8.2) */
    HV_SIMD_SHA512,  /* FEAT_SHA512: SHA512H/H2/SU0/SU1 (v8.2) */
    HV_SIMD_FCADD,   /* FEAT_ComplxNum: FCADD (v8.3) */
    HV_SIMD_FCMLA,   /* FEAT_ComplxNum: FCMLA (v8.3) */
    HV_SIMD_FP16,    /* FEAT_FullFP16: half arithmetic and conversions (v8.2) */
} HvSimdFamily;

typedef struct HvSimdOp {
    HvSimdFamily family;
    int q;       /* 128-bit vector form (bit 30) */
    int size;    /* size/esz field: 0 = 8-bit, 1 = 16-bit, 2 = 32-bit, 3 = 64-bit */
    int op;      /* family-specific: CRC size, arithmetic opcode, rounding mode... */
    int index;   /* by-element index */
    int rot;     /* FCMLA rotation */
    int imm;     /* XAR imm6 */
    uint8_t rd, rn, rm, ra;
} HvSimdOp;

/* Decode into `op`. Returns 1 if this is one of the families above. */
int hv_simd_decode(uint32_t insn, HvSimdOp *op);

/* Whether the translator has to hand this instruction over at all: true for the
   families above on a host that lacks them, false where the host can run it. */
int hv_simd_needs_help(uint32_t insn);

/* One instruction, exactly as the guest would have executed it: GPRs from
   cpu->x[], vectors from cpu->v[], FPCR/FPSR as they stand. Raises an
   undefined-instruction exception for anything it cannot do. Returns 1 if it
   took an exception (the block must leave), 0 otherwise. */
int hv_simd_emulate(HvCpu *cpu, uint32_t insn, uint64_t pc);

/* One instruction per family that the host can try, for probing and for the
   differential test. NULL when the family has no single probe instruction. */
uint32_t hv_simd_probe_insn(HvSimdFamily family);

const char *hv_simd_family_name(HvSimdFamily family);

#endif
