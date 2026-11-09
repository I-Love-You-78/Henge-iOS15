#include "vm.h"
#include "pvg_lite.h"

#include <TargetConditionals.h>
#include <CoreFoundation/CoreFoundation.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <stdio.h>
#include <time.h>

#include "usb_host.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_mac.h"
#include "classify.h"
#include "dispatch.h"
#include "hostcaps.h"
#include "net_slirp.h"
#include "pl011.h"
#include "simd.h"

static HvMachine m;
static HvCpu cpus[HV_MAX_CPUS]; /* the board takes them as an array */
static int ncpus = 1;
static HvVmapple board;
/* UI display changes may arrive before or during the background machine creation. */
static os_unfair_lock display_lock = OS_UNFAIR_LOCK_INIT;
static CFTypeRef display_layer;
static int display_layer_set;
static _Atomic(HvPvgLite *) display_pvg; /* published only when the board is ready */
static HvDisk disk, aux;
static int net_on;
static void (*extra_poll)(void);
static void (*exit_cb)(int);

HvMachine *hv_vm_machine(void) { return &m; }
HvVmapple *hv_vm_board(void) { return &board; }
HvCpu *hv_vm_cpu(int index) { return index >= 0 && index < ncpus ? &cpus[index] : NULL; }
int hv_vm_ncpus(void) { return ncpus; }

/* arm64 boot_args: {u16 Revision, u16 Version, u64 virtBase, physBase, memSize,
   topOfKernelData, Video {baseAddr, display, rowBytes, width, height, depth}, ...}.
   Found by scanning page starts for one whose physBase is the RAM base. */
int hv_vm_boot_display(HvBootDisplay *d) {
    /* What the guest's GPU has drawn, once it has drawn anything. */
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (!pvg) return -1;
    const uint8_t *frame;
    uint32_t fw, fh;
    if (hv_pvg_lite_frame(pvg, &frame, &fw, &fh) == 0) {
        d->pixels = (uint8_t *)frame;
        d->width = fw, d->height = fh, d->stride = fw * 4;
        return 0;
    }
    static uint64_t found; /* RAM offset of boot_args */
    uint8_t *ram = board.ram;
    uint64_t size = board.ram_size;
    if (!ram) return -1;
    static double last_scan;
    for (int pass = 0; pass < 2; pass++) {
        if (!found || pass) {
            /* A full scan at most every 2 s; boot_args sits low in RAM
               (iBoot puts it near 0x78ed0000), so the first GB is enough. */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            double now = ts.tv_sec + ts.tv_nsec * 1e-9;
            if (now - last_scan < 2) return -1;
            last_scan = now;
            if (pass && found) found = 0;
            uint64_t limit = size < (1ull << 30) ? size : 1ull << 30;
            for (uint64_t off = 0; off + 0x1000 <= limit && !found; off += 0x4000) {
                const uint64_t *q = (const uint64_t *)(ram + off);
                const uint16_t *h = (const uint16_t *)(ram + off);
                if (h[0] < 2 || h[0] > 3 || h[1] < 1 || h[1] > 3) continue; /* boot_args: revision, version */
                if (q[1] >> 40 != 0xfffffe || q[2] < VMAPPLE_MEM || q[2] >= VMAPPLE_MEM + size || q[3] == 0 || q[3] > size) continue;
                uint64_t base = q[5], row = q[7], w = q[8], hh = q[9], depth = q[10];
                if (w < 320 || w > 8192 || hh < 200 || hh > 8192 || row < w * 4 || (depth & 0xff) != 32) continue;
                if (base < VMAPPLE_MEM || base - VMAPPLE_MEM + row * hh > size) continue;
                found = off;
            }
        }
        if (!found) return -1;
        const uint64_t *q = (const uint64_t *)(ram + found);
        uint64_t base = q[5], row = q[7], w = q[8], hh = q[9];
        if (q[1] >> 40 == 0xfffffe && w >= 320 && w <= 8192 && hh >= 200 && hh <= 8192 && base >= VMAPPLE_MEM &&
            base - VMAPPLE_MEM + row * hh <= size) {
            d->pixels = ram + (base - VMAPPLE_MEM);
            d->width = (uint32_t)w, d->height = (uint32_t)hh, d->stride = (uint32_t)row;
            return 0;
        }
    }
    return -1;
}
void hv_vm_set_poll(void (*poll)(void)) { extra_poll = poll; }

const char *hv_vm_reason_name(int r) {
    switch (r) {
    case HV_EXIT_HALT: return "halt";
    case HV_EXIT_WFI: return "wfi with nothing to wake it";
    case HV_EXIT_UNSUPPORTED: return "unsupported";
    case HV_EXIT_FAULT_LOOP: return "fault on the exception vector";
    default: return "other";
    }
}

/* Host-side device work on CPU 0's thread each tick. */
/* HV_PATCH_STR="old=new[;old=new]": strings to rewrite in guest RAM, a little
   after the restore environment has booted. Equal lengths only, and only the
   bytes that differ move, so a vCPU never sees half a path. `old=new` with both
   halves the same is a probe: it only reports whether the string is there.

   Why this exists: in the restore environment here the root is the ramdisk and it
   is mounted read-only, so `restored` cannot create the mount point it wants for
   the iSC Preboot volume ("unable to create mount directory: 30", errno 30 =
   EROFS) and drops out with "failed to update device firmware pre FDR" at 7%.
   Pointing that base at a volume that is already mounted read-write (the
   Hardware volume, /mnt6) lets the step run; the volume mounted there is still
   the iSC Preboot one, so what gets written is unchanged. */
static char patch_pairs[4][2][64];
static int patch_pairs_n;
static int patch_attempts;
static int patch_done;
static int iboot_done;   /* iBoot has handed the machine to the kernel */
static char watch_buf[64];
static size_t watch_len;

/* The restore daemon prints these before it reaches the firmware step. */
static double patch_next;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void patch_read_spec(void) {
    const char *spec = getenv("HV_PATCH_STR");
    if (!spec || !*spec) return;
    char buf[4 * 2 * 64];
    snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *item = strtok_r(buf, ";", &save); item && patch_pairs_n < 4; item = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(item, '=');
        if (!eq) continue;
        size_t ol = (size_t)(eq - item), nl = strlen(eq + 1);
        if (ol == 0 || ol != nl || ol >= sizeof patch_pairs[0][0]) {
            fprintf(stderr, "hengevm: HV_PATCH_STR: '%s' needs equal-length halves under 64 bytes\n", item);
            continue;
        }
        memcpy(patch_pairs[patch_pairs_n][0], item, ol);
        patch_pairs[patch_pairs_n][0][ol] = 0;
        memcpy(patch_pairs[patch_pairs_n][1], eq + 1, nl + 1);
        patch_pairs_n++;
    }
    if (patch_pairs_n)
        fprintf(stderr, "hengevm: HV_PATCH_STR armed: %s -> %s\n", patch_pairs[0][0], patch_pairs[0][1]);
}

/* One pass over guest RAM: counts, and rewrites where the halves differ, so
   `from=from` works as a probe that only reports whether a string is there. */
static void patch_guest_strings(int attempt) {
    uint8_t *ram = board.ram;
    uint64_t size = board.ram_size;
    if (!ram || !size) return;
    int counts[4] = {0, 0, 0, 0};
    for (int i = 0; i < patch_pairs_n; i++) {
        const char *from = patch_pairs[i][0], *to = patch_pairs[i][1];
        size_t len = strlen(from);
        int changing = memcmp(from, to, len) != 0;
        uint64_t at = 0;
        while (at + len <= size) {
            uint8_t *p = memchr(ram + at, (unsigned char)from[0], (size_t)(size - at - len + 1));
            if (!p) break;
            if (memcmp(p, from, len) == 0) {
                counts[i]++;
                if (changing) {
                    for (size_t k = len; k-- > 0;)
                        if (p[k] != (unsigned char)to[k]) p[k] = (unsigned char)to[k]; /* one byte at a time */
                }
            }
            at = (uint64_t)(p - ram) + len;
        }
    }
    /* Say what each pair found; an attempt that found nothing is worth one line
       for the first few tries, or we cannot tell a wrong literal from a lost one. */
    if (board.m && board.m->log) {
        fprintf(stderr, "hengevm: HV_PATCH_STR attempt %d:", attempt);
        for (int i = 0; i < patch_pairs_n; i++) fprintf(stderr, " %s=%d", patch_pairs[i][0], counts[i]);
        fprintf(stderr, "\n");
    }
    for (int i = 0; i < patch_pairs_n; i++) {
        if (!counts[i] || memcmp(patch_pairs[i][0], patch_pairs[i][1], strlen(patch_pairs[i][0])) == 0) continue;
        patch_done = 1;
        if (board.m && board.m->log)
            fprintf(stderr, "hengevm: patched %d guest string(s) %s -> %s\n", counts[i], patch_pairs[i][0], patch_pairs[i][1]);
    }
}

/* Called for every byte the guest writes to its UART: it is how we know iBoot
   has handed over to the kernel. Patching before that rewrites strings inside
   iBoot (4 of them are its own "mnt5" at 8 s) and iBoot panics at :987. */
static void on_serial(char c) {
    if (!patch_pairs_n && !getenv("HV_RAM_DUMP")) return;
    if (watch_len + 1 < sizeof watch_buf) {
        watch_buf[watch_len++] = c;
    } else {
        memmove(watch_buf, watch_buf + 1, sizeof watch_buf - 2);
        watch_buf[sizeof watch_buf - 2] = c;
        watch_len = sizeof watch_buf - 1;
    }
    watch_buf[watch_len] = 0;
    static const char *const handoff = "End of iBootStage2 serial output";
    if (!iboot_done && strstr(watch_buf, handoff)) {
        iboot_done = 1;
        patch_next = now_seconds() + 3.0;
        if (board.m && board.m->log) fprintf(stderr, "hengevm: iBoot handed over; guest RAM is ours from here\n");
    }
}

/* HV_RAM_GREP="pat1;pat2...": with HV_RAM_DUMP=<file>, write a window around
   every occurrence of each pattern found in guest RAM, so the log file holds the
   text that matters (kernel mount messages, restored's own strings) without a
   1.5 GB dump. */
static char grep_pats[4][48];
static int grep_pats_n;

static void grep_read_spec(void) {
    const char *spec = getenv("HV_RAM_GREP");
    if (!spec || !*spec) return;
    char buf[4 * 48];
    snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *item = strtok_r(buf, ";", &save); item && grep_pats_n < 4; item = strtok_r(NULL, ";", &save)) {
        if (!*item) continue;
        snprintf(grep_pats[grep_pats_n], sizeof grep_pats[0], "%s", item);
        grep_pats_n++;
    }
    if (grep_pats_n) fprintf(stderr, "hengevm: HV_RAM_GREP armed: %d pattern(s)\n", grep_pats_n);
}

/* A window that starts at the beginning of the line the hit is on: the kernel's
   log lines run to a newline, so a hit is usually mid-line. */
static size_t window_start(const uint8_t *ram, uint64_t off, uint64_t size, uint64_t back) {
    uint64_t from = off > back ? off - back : 0;
    for (uint64_t at = off; at > from; at--)
        if (ram[at - 1] == '\n') return at;
    (void)size;
    return from;
}

static void dump_ram_grep(FILE *f, const char *path) {
    uint8_t *ram = board.ram;
    uint64_t size = board.ram_size;
    if (!ram || !size) return;
    int written = 0;
    for (int i = 0; i < grep_pats_n; i++) {
        const char *pat = grep_pats[i];
        size_t len = strlen(pat);
        int hits = 0;
        uint64_t at = 0;
        while (at + len <= size && hits < 24) {
            uint8_t *p = memchr(ram + at, (unsigned char)pat[0], (size_t)(size - at - len + 1));
            if (!p) break;
            uint64_t off = (uint64_t)(p - ram);
            if (memcmp(p, pat, len) == 0) {
                uint64_t from = window_start(ram, off, size, 256);
                uint64_t to = from + 2048;
                if (to > size) to = size;
                fprintf(f, "\n=== %s @ram+0x%llx ===\n", pat, (unsigned long long)off);
                for (uint64_t k = from; k < to; k++) {
                    uint8_t c = ram[k];
                    if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f)) fputc(c, f);
                    else fputc('.', f);
                }
                hits++;
                written += (int)(to - from);
            }
            at = off + len;
        }
    }
    fprintf(stderr, "hengevm: HV_RAM_GREP: %d bytes of context -> %s\n", written, path);
}
static void dump_ram_region(int attempt) {
    const char *path = getenv("HV_RAM_DUMP");
    if (!path || !*path) return;
    /* HV_RAM_DUMP_AT: which pass dumps (10 by default, about 45 s after iBoot
       hands over, just before the restore gives up at 7%). Later passes see the
       failure's own log lines, so which pass matters. */
    const char *at = getenv("HV_RAM_DUMP_AT");
    int want = at && atoi(at) > 0 ? atoi(at) : 10;
    if (attempt != want) return;
    if (grep_pats_n) {
        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "hengevm: HV_RAM_DUMP: cannot write %s: %s\n", path, strerror(errno));
            return;
        }
        dump_ram_grep(f, path);
        fclose(f);
        return;
    }
    const char *marker = getenv("HV_RAM_MARK");
    if (!marker || !*marker) marker = "Darwin Kernel";
    uint8_t *ram = board.ram;
    uint64_t size = board.ram_size;
    if (!ram || !size) return;
    size_t mlen = strlen(marker);
    uint64_t last = 0;
    int have = 0;
    for (uint64_t at = 0; at + mlen <= size;) {
        uint8_t *p = memchr(ram + at, (unsigned char)marker[0], (size_t)(size - at - mlen + 1));
        if (!p) break;
        if (memcmp(p, marker, mlen) == 0) last = (uint64_t)(p - ram), have = 1;
        at = (uint64_t)(p - ram) + mlen;
    }
    if (!have) {
        fprintf(stderr, "hengevm: HV_RAM_DUMP: '%s' is not in guest RAM\n", marker);
        return;
    }
    uint64_t len = 1024 * 1024;
    const char *kb = getenv("HV_RAM_DUMP_KB");
    if (kb && atoi(kb) > 0) len = (uint64_t)atoi(kb) * 1024;
    uint64_t from = last > (64 << 10) ? last - (64 << 10) : 0;
    if (from + len > size) len = size - from;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "hengevm: HV_RAM_DUMP: cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    size_t n = fwrite(ram + from, 1, (size_t)len, f);
    fclose(f);
    fprintf(stderr, "hengevm: HV_RAM_DUMP: %zu bytes from ram+%llx ('%s' at %llx) -> %s\n", n,
            (unsigned long long)from, marker, (unsigned long long)last, path);
}

static int scan_active(void) {
    return iboot_done && ((patch_pairs_n && !patch_done) || getenv("HV_RAM_DUMP") != NULL);
}

static void board_poll(void) {
    if (extra_poll) extra_poll();
    if (net_on) hv_net_poll();
    if (scan_active() && patch_attempts < 20) {
        double now = now_seconds();
        /* The scans start 3 s after iBoot hands over (see on_serial) and repeat
           every 5 s: restored's own strings only show up as its pages arrive,
           and the step that fails runs about 25 s after the handover. */
        if (patch_next == 0) patch_next = now + 3.0;
        if (now >= patch_next) {
            patch_attempts++;
            if (patch_pairs_n) patch_guest_strings(patch_attempts);
            dump_ram_region(patch_attempts);
            patch_next = now + 5.0;
        }
    }
}
static int fail(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
    return -1;
}

/* A branch landing where no translation is (an illegal instruction, or a fault
   fetching one): say where, and which direct branches in translated code lead
   there, for a crash report cannot. */
static struct sigaction prior_ill, prior_bus, prior_segv;
static void on_ill(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = ctx;
    uint64_t pc = uc->uc_mcontext->__ss.__pc, rx = (uint64_t)(uintptr_t)m.jit.base, size = (uint64_t)m.jit.capacity * 4;
    uint64_t used = (uint64_t)m.jit.used * 4;
    int in_rx = pc >= rx && pc < rx + size;
    int fetch = (uint64_t)(uintptr_t)info->si_addr == pc;
    if (!(sig == SIGILL || in_rx || fetch)) {
        /* A fault in Henge's own code (a bad pointer, a stack) rather than in a
           translated block: still worth one line, or the crash looks silent. */
        fprintf(stderr, "hengevm: signal %d at pc %llx (outside the JIT region), addr %p, lr %llx\n", sig,
                (unsigned long long)pc, info->si_addr, (unsigned long long)uc->uc_mcontext->__ss.__lr);
    }
    if (sig == SIGILL || in_rx || fetch) {
        fprintf(stderr, "hengevm: signal %d in translated code: pc %llx (rx %llx size %llx used %llx, pc is rx%+lld), full flushes %llu, lr %llx, addr %p\n",
                sig, (unsigned long long)pc, (unsigned long long)rx, (unsigned long long)size, (unsigned long long)used, (long long)(pc - rx),
                (unsigned long long)m.stats.block_flushes, (unsigned long long)uc->uc_mcontext->__ss.__lr, info->si_addr);
        for (int i = 0; i < 29; i++) fprintf(stderr, "x%d=%llx%c", i, (unsigned long long)uc->uc_mcontext->__ss.__x[i], i % 6 == 5 ? '\n' : ' ');
        fprintf(stderr, "\n");
        fprintf(stderr, "sp=%llx ram %llx size %llx\n", (unsigned long long)uc->uc_mcontext->__ss.__sp, (unsigned long long)(uintptr_t)board.ram, (unsigned long long)board.ram_size);
        {
            uint64_t x7 = uc->uc_mcontext->__ss.__x[7], x28 = uc->uc_mcontext->__ss.__x[28];
            if (x7 >= x28 && x7 < x28 + sizeof(HvCpu)) {
                const uint64_t *ent = (const uint64_t *)(uintptr_t)(x7 + 0x500);
                fprintf(stderr, "tlb entry at +%llx:", (unsigned long long)(x7 - x28));
                for (int i = 0; i < 16; i++) fprintf(stderr, " %llx", (unsigned long long)ent[i]);
                fprintf(stderr, "\n");
            }
        }
        for (int i = 0; i < ncpus; i++)
            fprintf(stderr, "  cpu %d: guest pc %llx el%u miss site %llx\n", i, (unsigned long long)cpus[i].pc, cpus[i].el, (unsigned long long)cpus[i].miss_site);
        if (in_rx) { /* what the host refused to run, named */
            uint32_t w = *(const uint32_t *)(uintptr_t)pc;
            fprintf(stderr, "  the word that trapped is %08x (%s)\n", w, hv_insn_kind_name(hv_classify(w)));
            HvSimdOp op;
            if (hv_simd_decode(w, &op)) fprintf(stderr, "  it is a %s instruction this host lacks\n", hv_simd_family_name(op.family));
            else if ((w & 0xffe0001fu) == 0xd5030000u) fprintf(stderr, "  it is a hint or barrier: see hv_barrier_word\n");
        }
        if (in_rx) { /* the code it ran up to here, as it stands */
            uint64_t from = pc - 0x300 < rx ? rx : pc - 0x300;
            fprintf(stderr, "  words from rx+%llx:", (unsigned long long)(from - rx));
            for (uint64_t at = from; at < pc + 0x600 && at < rx + size; at += 4) {
                if (((at - from) & 0x1f) == 0) fprintf(stderr, "\n   ");
                fprintf(stderr, " %08x", *(const uint32_t *)(uintptr_t)at);
            }
            fprintf(stderr, "\n");
        }
        { /* x15 after a chained exit's check: the literal it loaded. Which exits, and where do they go now? */
            uint64_t x15 = uc->uc_mcontext->__ss.__x[15];
            const uint32_t *w15 = (const uint32_t *)(uintptr_t)rx;
            uint64_t n15 = used / 4 < m.jit.capacity ? used / 4 : m.jit.capacity;
            int shown = 0;
            for (uint64_t k = 0; k + 8 < n15 && shown < 12; k++) {
                if ((w15[k] & 0xff00001fu) != 0x5800000fu) continue; /* ldr x15, literal */
                int64_t lo = (int64_t)(int32_t)(w15[k] << 8) >> 13;
                uint64_t at = k + (uint64_t)lo;
                if (at + 1 >= n15) continue;
                uint64_t lit = w15[at] | (uint64_t)w15[at + 1] << 32;
                if (lit != x15) continue;
                /* eor x6; orr; cbnz; B */
                uint32_t b = w15[k + 4];
                if ((b & 0xfc000000u) != 0x14000000u) continue;
                int64_t off = (int64_t)(b << 6) >> 6;
                fprintf(stderr, "  exit at rx+%llx: B word %08x to rx+%llx\n", (unsigned long long)(k + 4) * 4, b, (unsigned long long)((int64_t)(k + 4) + off) * 4);
                shown++;
            }
        }
        if (!in_rx || pc >= rx + used) {
            /* Who branches there? */
            const uint32_t *w = (const uint32_t *)(uintptr_t)rx;
            int found = 0;
            uint64_t words = used / 4 < m.jit.capacity ? used / 4 : m.jit.capacity;
            for (uint64_t k = 0; k < words && found < 8; k++) {
                uint32_t i = w[k];
                int64_t off;
                if ((i & 0x7c000000u) == 0x14000000u) off = (int64_t)(i << 6) >> 6;
                else if ((i & 0xff000010u) == 0x54000000u || (i & 0x7e000000u) == 0x34000000u) off = (int64_t)(int32_t)(i << 8) >> 13;
                else if ((i & 0x7e000000u) == 0x36000000u) off = (int64_t)(int32_t)(i << 13) >> 18;
                else continue;
                if (rx + k * 4 + (uint64_t)(off * 4) != pc && rx + k * 4 + (uint64_t)(off * 4) != pc - 4) continue;
                found++;
                fprintf(stderr, "  branch at rx+%llx (word %08x) leads there; around it:", (unsigned long long)k * 4, i);
                for (int64_t d = -6; d <= 3; d++)
                    if ((int64_t)k + d >= 0 && (uint64_t)((int64_t)k + d) < words) fprintf(stderr, " %08x", w[k + d]);
                fprintf(stderr, "\n");
            }
            if (!found) fprintf(stderr, "  no direct branch in %llu MB of code leads there\n", (unsigned long long)(used >> 20));
            /* Which slot or cache entry holds it? */
            for (uint32_t k = 0; k < m.max_slots; k++)
                if (m.slots && m.slots[k] == pc) fprintf(stderr, "  chain slot %u holds it\n", k);
        }
        fflush(stderr);
    }
    struct sigaction *prior = sig == SIGILL ? &prior_ill : sig == SIGBUS ? &prior_bus : &prior_segv;
    sigaction(sig, prior, NULL);
}

void (*hv_prof_register)(void);

void hv_vm_set_display_layer(void *layer) {
    CFTypeRef retained = layer ? CFRetain(layer) : NULL;
    os_unfair_lock_lock(&display_lock);
    CFTypeRef previous = display_layer;
    display_layer = retained;
    display_layer_set = 1;
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (pvg) hv_pvg_lite_set_layer(pvg, layer);
    os_unfair_lock_unlock(&display_lock);
    if (previous) CFRelease(previous);
}

uint64_t hv_vm_gpu_frames(void) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    return pvg ? hv_pvg_lite_gpu_frames(pvg) : 0;
}

int hv_vm_create(const HvVmConfig *c, char *err, size_t errlen) {
    if (!c->rom || !c->disk || !c->aux) return fail(err, errlen, "The ROM, disk and auxiliary storage are all needed.");
    /* Which Armv8.1+ features this host has, resolved here on one thread: each
       probe runs an instruction under a SIGILL guard, which must not happen once
       the vCPUs are running (a fault on another thread would land in this
       thread's handler and jump into this thread's stack). */
    hv_host_features_resolve();
    fprintf(stderr, "henge: host features: %s\n", hv_host_feature_summary());
    struct sigaction ill = {.sa_flags = SA_SIGINFO | SA_ONSTACK};
    ill.sa_sigaction = on_ill;
    sigaction(SIGILL, &ill, &prior_ill);
    sigaction(SIGBUS, &ill, &prior_bus);
    sigaction(SIGSEGV, &ill, &prior_segv);
    signal(SIGPIPE, SIG_IGN); /* a guest connection closing at the far end must not end the machine */
    if (hv_disk_open(&disk, c->disk, 0, 0) != 0) return fail(err, errlen, "Cannot open the disk image.");
    if (hv_disk_open(&aux, c->aux, c->aux_offset ? c->aux_offset : 16384, 0) != 0)
        return fail(err, errlen, "Cannot open the auxiliary storage.");
    size_t jit = c->jit_bytes ? c->jit_bytes : (size_t)768 << 20;
    int r = c->jit_rx ? hv_machine_init_jit(&m, c->jit_rx, c->jit_rw, jit) : hv_machine_init(&m, jit);
    if (r != 0) return fail(err, errlen, "Cannot set up the JIT region.");
    m.log = c->log;
    m.gen_checks = getenv("HV_JIT_POISON") != NULL;
    if (getenv("HV_SLOW_MEM")) m.slow_mem = 1; /* debugging: every load and store through the helper */
    ncpus = c->ncpus < 1 ? 1 : c->ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : c->ncpus;
    m.big_lock_on = ncpus > 1 && getenv("HV_BIG_LOCK"); /* debugging: one CPU in guest code at a time */
    HvCfgParams cfg = {.nr_cpus = (uint32_t)ncpus, .ecid = c->ecid, .seed = c->seed, .mac_en0 = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01}};
    if (c->installer) cfg.run_installer1 = cfg.run_installer2 = 1;
    if (getenv("HV_RUN_INSTALLER")) { /* experiment: "a,b" for the two installer words */
        unsigned a = 0, b2 = 0;
        sscanf(getenv("HV_RUN_INSTALLER"), "%u,%u", &a, &b2);
        cfg.run_installer1 = a, cfg.run_installer2 = b2;
    }
    static const uint8_t mac[6] = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01};
    if (hv_vmapple_create(&board, &m, cpus, (c->ram_mb ? c->ram_mb : 2048) << 20, &disk, &aux, &cfg, mac) != 0)
        return fail(err, errlen, "Cannot build the machine (not enough memory for the guest's RAM?).");
    if (hv_vmapple_load_rom(&board, c->rom) != 0) return fail(err, errlen, "Cannot read the ROM (AVPBooter.vmapple2.bin).");
    hv_vmapple_reset_cpu(&board);
    if (board.net && !c->no_net) {
        if (hv_net_attach(&m, board.net) != 0) fprintf(stderr, "hengevm: user-mode network unavailable\n");
        else net_on = 1;
    }
    if (board.snd && !c->no_sound) hv_audio_attach(board.snd);
    board.poll = board_poll;
    /* Optional: rewrite strings in guest RAM once the restore daemon is up (see
       HV_PATCH_STR above), which is how the read-only root stops being fatal. */
    patch_read_spec();
    grep_read_spec();
    if (patch_pairs_n || grep_pats_n || getenv("HV_RAM_DUMP")) hv_pl011_watch = on_serial;
    if (board.vusb && getenv("HV_VUSB_PROBE")) hv_usb_probe_start(board.vusb);
    // The setter and publication share this lock: an older preboot target can
    // never overwrite a monitor change made while the board was being built.
    os_unfair_lock_lock(&display_lock);
    if (display_layer_set && board.pvg) hv_pvg_lite_set_layer(board.pvg, (void *)display_layer);
    atomic_store_explicit(&display_pvg, board.pvg, memory_order_release);
    os_unfair_lock_unlock(&display_lock);
    return 0;
}

static void *kicker_thread(void *unused) {
    (void)unused;
    for (;;) {
        usleep(1000);
        hv_machine_kick_all(&m);
    }
    return NULL;
}

/* A thread that runs guest code. While a block runs its SP register holds the
   guest's SP, a guest address, so the kernel must never push a signal frame
   there: other threads take the asynchronous signals, and a handler that does
   run here (a fault) gets a stack of its own. */
void hv_vm_prepare_cpu_thread(void) {
    static __thread int done;
    if (done) return;
    done = 1;
    if (hv_prof_register) hv_prof_register();
    stack_t ss = {.ss_sp = malloc(1u << 16), .ss_size = 1u << 16, .ss_flags = 0};
    if (ss.ss_sp) sigaltstack(&ss, NULL);
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGSEGV), sigdelset(&set, SIGBUS), sigdelset(&set, SIGILL), sigdelset(&set, SIGTRAP), sigdelset(&set, SIGFPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

/* iOS parks default-QoS threads on efficiency cores readily; the guest's CPUs are what the user waits on. */
void hv_vm_cpu_qos(void) {
    hv_vm_prepare_cpu_thread();
    /* HV_QOS=ui|init|default (testing): the class the guest's CPUs run at. */
    const char *q = getenv("HV_QOS");
#if TARGET_OS_IPHONE
    qos_class_t c = QOS_CLASS_USER_INITIATED;
#else
    qos_class_t c = QOS_CLASS_DEFAULT;
#endif
    if (q && !strcmp(q, "ui")) c = QOS_CLASS_USER_INTERACTIVE;
    else if (q && !strcmp(q, "init")) c = QOS_CLASS_USER_INITIATED;
    else if (q && !strcmp(q, "default")) c = QOS_CLASS_DEFAULT;
    if (c != QOS_CLASS_DEFAULT) pthread_set_qos_class_self_np(c, 0);
}

/* CPUs 1 and up: run whenever PSCI CPU_ON starts them; the machine stops with CPU 0. */
static void *secondary_thread(void *arg) {
    HvCpu *c = arg;
    hv_vm_cpu_qos();
    int r = hv_run(c);
    if (!c->halted)
        fprintf(stderr, "hengevm: cpu %u stopped (%s) at pc %llx el%u\n", c->index, hv_vm_reason_name(r),
                (unsigned long long)c->pc, c->el);
    return NULL;
}

void hv_vm_flush_disks(void) {
    hv_disk_flush(&disk);
    hv_disk_flush(&aux);
}

int hv_vm_run_cpu0(void) {
    return hv_run(&cpus[0]);
}

static void *cpu0_thread(void *unused) {
    (void)unused;
    hv_vm_cpu_qos();
    int r = hv_vm_run_cpu0();
    hv_vm_flush_disks();
    if (exit_cb) exit_cb(r);
    return NULL;
}

void hv_vm_start(int cpu0, void (*on_exit)(int reason)) {
    exit_cb = on_exit;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t t;
    if (cpu0) pthread_create(&t, &attr, cpu0_thread, NULL);
    for (int i = 1; i < ncpus; i++) pthread_create(&t, &attr, secondary_thread, &cpus[i]);
    pthread_create(&t, NULL, kicker_thread, NULL);
}

void hv_vm_stop(void) {
    for (int i = 0; i < ncpus; i++) cpus[i].halted = 3; /* the dispatchers stop at the next block */
    hv_machine_kick_all(&m);
    for (int i = 0; i < ncpus; i++) hv_cpu_wake(&cpus[i]);
}

void hv_vm_key(uint16_t code, int down) {
    hv_xhci_key_linux(board.usb, code, down);
}

void hv_vm_key_hid(uint8_t usage, int down) {
    hv_xhci_key(board.usb, usage, down);
}

void hv_vm_mouse_move(int dx, int dy) {
    if (dx) hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_X, dx);
    if (dy) hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_Y, dy);
    hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
}

void hv_vm_mouse_button(int button, int down) {
    static const uint16_t codes[3] = {HV_BTN_LEFT, HV_BTN_RIGHT, HV_BTN_MIDDLE};
    if (button < 0 || button > 2) return;
    hv_virtio_input_send(board.mouse, HV_EV_KEY, codes[button], down);
    hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
}

/* The guest takes a wheel event as 8 bits: -200 arrives as +56, the wrong way. So big moves go in pieces. */
static void wheel(uint16_t axis, int amount) {
    while (amount) {
        int piece = amount > 100 ? 100 : amount < -100 ? -100 : amount;
        hv_virtio_input_send(board.mouse, HV_EV_REL, axis, piece);
        hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
        amount -= piece;
    }
}

void hv_vm_mouse_wheel(int dy) { wheel(HV_REL_WHEEL, dy); }
void hv_vm_mouse_hwheel(int dx) { wheel(HV_REL_HWHEEL, dx); }

int hv_vm_pointer(int *x, int *y) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    const uint8_t *page = pvg ? hv_pvg_lite_display_page(pvg) : NULL;
    if (!page) return -1;
    uint32_t at = *(const volatile uint32_t *)(page + 0xe00);
    if (at == 0xffffffff) return -1;
    *x = (int)(at & 0xffff), *y = (int)(at >> 16);
    return 0;
}

int hv_vm_snapshot(uint8_t *bgra, int width, int height) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (!pvg || width <= 0 || height <= 0) return -1;
    return hv_pvg_lite_snapshot(pvg, bgra, (uint32_t)width, (uint32_t)height);
}
