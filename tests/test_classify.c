#include "classify.h"

#include <stdio.h>
#include <string.h>

extern const uint32_t hv_classify_cases[];
extern const uint32_t hv_classify_cases_end[];

static const HvInsnKind expected[] = {
    HV_INSN_COPY, HV_INSN_COPY, HV_INSN_COPY, HV_INSN_COPY, HV_INSN_COPY, HV_INSN_NOP,
    HV_INSN_PCREL, HV_INSN_PCREL,
    HV_INSN_BRANCH, HV_INSN_BRANCH, HV_INSN_BRANCH, HV_INSN_BRANCH, HV_INSN_BRANCH,
    HV_INSN_BRANCH_REG, HV_INSN_BRANCH_REG, HV_INSN_BRANCH_REG, HV_INSN_BRANCH_REG,
    HV_INSN_ERET,
    HV_INSN_LOAD_LITERAL, HV_INSN_LOAD_LITERAL,
    HV_INSN_MEMORY, HV_INSN_MEMORY, HV_INSN_MEMORY, HV_INSN_MEMORY, HV_INSN_MEMORY,
    HV_INSN_PREFETCH,
    HV_INSN_EXCEPTION, HV_INSN_EXCEPTION, HV_INSN_EXCEPTION,
    HV_INSN_SYSREG, HV_INSN_SYSREG, HV_INSN_SYSREG,
    HV_INSN_SYS, HV_INSN_SYS,
    HV_INSN_BARRIER, HV_INSN_BARRIER,
    HV_INSN_WAIT,
    HV_INSN_PAC, HV_INSN_PAC, HV_INSN_PAC, HV_INSN_PAC, HV_INSN_PAC,
    HV_INSN_UNDEFINED,
};

/* The hint and barrier spaces: what an older host does not decode must never be
   handed to it as it is. SB is the one that cost a restore. */
static const struct {
    uint32_t insn;
    HvInsnKind kind;
    uint32_t emit;   /* what the JIT emits, 0 if the kind decides */
    const char *what;
} special[] = {
    {0xd50330ff, HV_INSN_BARRIER, 0xd503201f, "SB"},
    {0xd50332ff, HV_INSN_BARRIER, 0xd503201f, "TSB"},
    {0xd50333ff, HV_INSN_BARRIER, 0xd503201f, "CLRBHB"},
    {0xd5033f9f, HV_INSN_BARRIER, 0xd5033f9f, "DSB SY"},
    {0xd5033fbf, HV_INSN_BARRIER, 0xd5033fbf, "DMB SY"},
    {0xd503319f, HV_INSN_BARRIER, 0xd503319f, "DSB OSHLD"},
    {0xd50331bf, HV_INSN_BARRIER, 0xd50331bf, "DMB OSHLD"},
    {0xd503349f, HV_INSN_BARRIER, 0xd5033f9f, "DSB #4 reserved -> SY"},
    {0xd503201f, HV_INSN_COPY, 0, "NOP"},
    {0xd503203f, HV_INSN_COPY, 0, "YIELD"},
    {0xd503209f, HV_INSN_COPY, 0, "SEV"},
    {0xd50320bf, HV_INSN_COPY, 0, "SEVL"},
    {0xd503205f, HV_INSN_WAIT, 0, "WFE"},
    {0xd503207f, HV_INSN_WAIT, 0, "WFI"},
    {0xd503241f, HV_INSN_NOP, 0xd503201f, "BTI"},
    {0xd503247f, HV_INSN_NOP, 0xd503201f, "BTI jc"},
    {0xd503229f, HV_INSN_NOP, 0xd503201f, "CSDB"},
    {0xd503221f, HV_INSN_NOP, 0xd503201f, "ESB"},
    {0xd503233f, HV_INSN_PAC, 0, "PACIASP"},
    {0xd50320ff, HV_INSN_PAC, 0, "XPACLRI"},
    {0xd5033fdf, HV_INSN_BARRIER, 0, "ISB (the translator ends the block)"},
    {0xd5033e5f, HV_INSN_BARRIER, 0, "CLREX (helper)"},
};

int main(void) {
    size_t count = (size_t)(hv_classify_cases_end - hv_classify_cases);
    size_t want = sizeof expected / sizeof expected[0];
    if (count != want) {
        printf("case count %zu does not match %zu expectations\n", count, want);
        return 1;
    }
    int failures = 0;
    for (size_t i = 0; i < count; i++) {
        HvInsnKind got = hv_classify(hv_classify_cases[i]);
        if (got != expected[i]) {
            printf("case %zu: %08x is %s, expected %s\n", i, hv_classify_cases[i],
                   hv_insn_kind_name(got), hv_insn_kind_name(expected[i]));
            failures++;
        }
    }
    for (size_t i = 0; i < sizeof special / sizeof special[0]; i++) {
        HvInsnKind got = hv_classify(special[i].insn);
        if (got != special[i].kind) {
            printf("%s (%08x): classified %s, expected %s\n", special[i].what, special[i].insn,
                   hv_insn_kind_name(got), hv_insn_kind_name(special[i].kind));
            failures++;
            continue;
        }
        if (got != HV_INSN_BARRIER || special[i].emit == 0) continue;
        uint32_t emitted = hv_barrier_word(special[i].insn);
        if (emitted != special[i].emit) {
            printf("%s (%08x): emits %08x, expected %08x\n", special[i].what, special[i].insn, emitted,
                   special[i].emit);
            failures++;
        }
    }
    printf("%zu cases plus %zu hint/barrier cases, %d failures\n", count, sizeof special / sizeof special[0],
           failures);
    return failures != 0;
}
