#ifndef HENGEVM_HOSTCAPS_H
#define HENGEVM_HOSTCAPS_H

/* What the *host* CPU can run beyond Armv8.0.
 *
 * The guest is arm64 macOS, which Apple builds for Armv8.5: its code contains
 * Armv8.1+ instructions that the translator normally copies into a block
 * unchanged. On an A9 (iPhone 6s/SE, iOS 15) those instructions are
 * unallocated, and executing one is a SIGILL that kills Henge. So the
 * translator asks here first and sends anything the host cannot run to the
 * C slow path, which emulates it (core/memop.c).
 *
 * Answers are cached: call them as often as you like. */

/* FEAT_LSE (Armv8.1): LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN,
   SWP, CAS, CASP; LDAPR is FEAT_LRCPC (v8.3) and is emulated alongside. */
int hv_host_has_lse(void);

/* FEAT_LRCPC2 (Armv8.4): LDAPUR/STLUR (the imm9 ordered forms). */
int hv_host_has_lrcpc2(void);

/* Informational, for the log line: FEAT_DotProd and FEAT_FP16 arithmetic.
   Neither is emulated yet, so a guest that runs them still faults; knowing the
   host's answer makes such a report readable. */
int hv_host_has_dotprod(void);
int hv_host_has_fp16(void);

/* Records that a feature had to be probed rather than read from a sysctl. */
void hv_host_note_probe(void);

/* "host features: LSE=yes LRCPC2=yes DotProd=yes FP16=yes (probed)" for the log. */
const char *hv_host_feature_summary(void);

#endif
