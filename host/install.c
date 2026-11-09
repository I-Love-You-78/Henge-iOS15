/* Installing macOS on a fresh virtual Mac, the way Virtualization.framework's
   VZMacOSInstaller does: the machine boots its ROM in DFU mode and shows up
   as a USB device, and a standard restore (personalized with Apple's signing
   server for this machine's ECID, then the restore OS writes the disk) runs
   over that link. The restore tools are the open-source libirecovery,
   usbmuxd and idevicerestore, unmodified apart from the boot-ROM exploit left
   out, talking to the guest through host/libusb_shim.c. Nothing of Apple's is
   decrypted or patched here: encrypted images go to the guest as they are, and
   the restore OS asks for its keys itself (the host just fetches the URL). */

#include "install.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <plist/plist.h>
#include <zip.h>

#include "idevicerestore/src/idevicerestore.h"
#include "usb_host.h"

int usbmuxd_main(int argc, char **argv);

static HvInstallProgress g_progress;
static void *g_progress_user;
static char g_ipsw[1024], g_cache[1024], g_mux[64];
static uint64_t g_ecid;
static volatile int g_result = 1; /* 1 running, 0 done, < 0 failed */

static void *mux_thread(void *arg) {
    (void)arg;
    char *argv[] = {"usbmuxd", "-f", "-p", "-P", "NONE", "-S", g_mux, "-C", g_cache, NULL};
    int argc = 9;
    if (getenv("HV_INSTALL_LOG")) {
        static char *v[] = {"usbmuxd", "-f", "-v", "-v", "-p", "-P", "NONE", "-S", NULL, "-C", NULL, NULL};
        v[8] = g_mux, v[10] = g_cache;
        usbmuxd_main(11, v);
    } else {
        usbmuxd_main(argc, argv);
    }
    fprintf(stderr, "install: usbmuxd stopped\n");
    return NULL;
}

static void progress(int step, double p, void *user) {
    (void)user;
    if (g_progress) g_progress(step, p, g_progress_user);
}

static void *restore_thread(void *arg) {
    (void)arg;
    struct idevicerestore_client_t *c = idevicerestore_client_new();
    if (!c) {
        g_result = -1;
        return NULL;
    }
    int flags = FLAG_ERASE;
    if (getenv("HV_INSTALL_LOG")) flags |= FLAG_DEBUG;
    idevicerestore_set_flags(c, flags);
    idevicerestore_set_ipsw(c, g_ipsw);
    idevicerestore_set_cache_path(c, g_cache);
    if (g_ecid) idevicerestore_set_ecid(c, g_ecid);
    idevicerestore_set_progress_callback(c, progress, NULL);
    int r = idevicerestore_start(c);
    fprintf(stderr, "install: restore finished: %d\n", r);
    idevicerestore_client_free(c);
    g_result = r == 0 ? 0 : -1;
    if (g_progress) g_progress(r == 0 ? HV_INSTALL_DONE : HV_INSTALL_FAILED, 1.0, g_progress_user);
    return NULL;
}

int hv_install_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid, HvInstallProgress cb,
                     void *user) {
    if (!usb) return -1;
    snprintf(g_ipsw, sizeof g_ipsw, "%s", ipsw);
    snprintf(g_cache, sizeof g_cache, "%s", workdir);
    /* A private usbmuxd on loopback; the restore tools find it through this. */
    int port = 27100 + (int)(getpid() % 800);
    snprintf(g_mux, sizeof g_mux, "127.0.0.1:%d", port);
    setenv("USBMUXD_SOCKET_ADDRESS", g_mux, 1);
    g_ecid = ecid;
    g_progress = cb;
    g_progress_user = user;
    hv_libusb_shim_attach(usb);
    pthread_t t;
    if (pthread_create(&t, NULL, mux_thread, NULL) != 0) return -1;
    pthread_detach(t);
    usleep(300000);
    if (pthread_create(&t, NULL, restore_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}

int hv_install_result(void) { return g_result; }

/* ---- what an image can install ------------------------------------------ */

/* Reads BuildManifest.plist out of an IPSW (a zip of 16-20 GB, so only that one
   member is read) and reports the version it holds and the product types it
   lists. libzip and libplist are already linked for the restore itself. */
int hv_install_check_ipsw(const char *ipsw, char *version, size_t vlen, char *products, size_t plen,
                          int *builds_virtual_mac) {
    if (version && vlen) version[0] = 0;
    if (products && plen) products[0] = 0;
    if (builds_virtual_mac) *builds_virtual_mac = 0;

    int err = 0;
    zip_t *z = zip_open(ipsw, ZIP_RDONLY, &err);
    if (!z) return -1;
    zip_file_t *f = zip_fopen(z, "BuildManifest.plist", 0);
    if (!f) { zip_close(z); return -2; }

    size_t cap = 64u << 20, len = 0;
    char *buf = malloc(cap);
    if (!buf) { zip_fclose(f); zip_close(z); return -3; }
    for (;;) {
        if (len == cap) {
            char *more = realloc(buf, cap * 2);
            if (!more) break;
            buf = more, cap *= 2;
        }
        zip_int64_t n = zip_fread(f, buf + len, (zip_uint64_t)(cap - len));
        if (n <= 0) break;
        len += (size_t)n;
    }
    zip_fclose(f);
    zip_close(z);

    plist_t root = NULL;
    plist_format_t format = PLIST_FORMAT_NONE;
    int rc = -4;
    if (len && plist_from_memory(buf, (uint32_t)len, &root, &format) == PLIST_ERR_SUCCESS && root) {
        rc = 0;
        /* "27.2 (26B123)": the build matters as much as the version. Apple ships
           more than one image per release, and only some of them carry a virtual
           Mac (27.2 build 26B8125 lists Mac17,10-13 only; 26B123 lists
           VirtualMac2,1). */
        {
            char ver[32] = "", build[32] = "";
            char *s = NULL;
            plist_t v = plist_dict_get_item(root, "ProductVersion");
            if (v) { plist_get_string_val(v, &s); if (s) { snprintf(ver, sizeof ver, "%s", s); free(s); s = NULL; } }
            plist_t b = plist_dict_get_item(root, "ProductBuildVersion");
            if (b) { plist_get_string_val(b, &s); if (s) { snprintf(build, sizeof build, "%s", s); free(s); } }
            if (version && vlen) snprintf(version, vlen, build[0] ? "%s (%s)" : "%s", ver, build);
        }
        plist_t types = plist_dict_get_item(root, "SupportedProductTypes");
        size_t at = 0;
        if (types && plist_get_node_type(types) == PLIST_ARRAY) {
            uint32_t n = plist_array_get_size(types);
            for (uint32_t i = 0; i < n; i++) {
                plist_t item = plist_array_get_item(types, i);
                char *s = NULL;
                if (!item || plist_get_node_type(item) != PLIST_STRING) continue;
                plist_get_string_val(item, &s);
                if (!s) continue;
                if (strcmp(s, "VirtualMac2,1") == 0 && builds_virtual_mac) *builds_virtual_mac = 1;
                if (products && at + strlen(s) + 2 < plen) {
                    at += (size_t)snprintf(products + at, plen - at, "%s%s", at ? "," : "", s);
                }
                free(s);
            }
        }
        plist_free(root);
    }
    free(buf);
    return rc;
}