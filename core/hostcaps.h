#ifndef HENGEVM_HOSTCAPS_H
#define HENGEVM_HOSTCAPS_H

/* What the *host* CPU can run beyond Armv8.0.
 *
 * The guest is arm64 macOS, and clang builds every arm64 macOS binary for
 * apple-m1 by default (clang/lib/Driver/ToolChains/Arch/AArch64.cpp returns
 * "apple-m1" for an aarch64 macOS triple), so guest code may contain Armv8.1 to
 * Armv8.5 instructions plus FEAT_FullFP16, FP16FML, DotProd, SHA3, LSE,
 * ComplxNum and JSCVT. An A9 has none of those; an A10 has CRC/RDM only; an A11
 * adds LSE and FP16; an A12 adds v8.3; an A13 adds DotProd and FP16FML. HengeVM
 * copies instructions into blocks on the assumption that the host can run them,
 * so anything else has to be detected and emulated (core/simd.c).
 *
 * Detection is per feature: the sysctl where Darwin names it, otherwise the
 * instruction is executed once under a SIGILL guard. Feature tables for a chip
 * model are an assumption; this is a measurement.
 *
 * Answers are cached: call them as often as you like. */

int hv_host_has_lse(void);     /* FEAT_LSE (v8.1): LDADD/SWP/CAS/CASP */
int hv_host_has_lrcpc2(void);  /* FEAT_LRCPC2 (v8.4): LDAPUR/STLUR */
int hv_host_has_crc32(void);   /* FEAT_CRC32: CRC32B/H/W/X and CRC32C* */
int hv_host_has_rdm(void);     /* FEAT_RDM (v8.1): SQRDMLAH/SQRDMLSH */
int hv_host_has_fp16(void);    /* FEAT_FullFP16 (v8.2): half arithmetic */
int hv_host_has_fhm(void);     /* FEAT_FP16FML (v8.2): FMLAL/FMLSL */
int hv_host_has_dotprod(void); /* FEAT_DotProd (v8.2): SDOT/UDOT */
int hv_host_has_sha3(void);    /* FEAT_SHA3 (v8.2): EOR3/RAX1/BCAX/XAR */
int hv_host_has_sha512(void);  /* FEAT_SHA512 (v8.2) */
int hv_host_has_fcma(void);    /* FEAT_ComplxNum (v8.3): FCADD/FCMLA */
int hv_host_has_jscvt(void);   /* FEAT_JSCVT (v8.3): FJCVTZS */

/* Resolve every feature now, on this thread. Called once while the machine is
   built, before any CPU thread exists: a probe installs a SIGILL handler and
   siglongjmps, which is only safe when nothing else can fault at the same time
   (the vCPUs would otherwise race each other through one handler). */
void hv_host_features_resolve(void);

/* Records that a feature had to be probed rather than read from a sysctl. */
void hv_host_note_probe(void);

/* "host features: LSE=no LRCPC2=no ..." for the log. */
const char *hv_host_feature_summary(void);

#endif
