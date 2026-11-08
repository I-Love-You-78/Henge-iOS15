/* The guest's virtio-net with no NAT behind it.
 *
 * Henge's networking needs libslirp and glib, which the iOS 15/A9 build does
 * not link (they are built from a Husk checkout, and the point of that build is
 * a toolchain-free CI). The NIC the board creates still has to answer: with no
 * user-mode network attached, the guest sees a link with nothing behind it and
 * macOS falls back to no network, which the installer does not need (it
 * downloads through the app, host side) and a booted Mac lives with.
 *
 * Sharing (host/share.c, a WebDAV server the guest would reach at 10.0.2.4)
 * rides on the same user-mode network, so it is unavailable here too. */
#include "net_slirp.h"

#include <stdio.h>

uint64_t hv_net_tx_frames, hv_net_rx_frames;

void hv_net_set_share_socket(const char *path) {
    (void)path;
}

int hv_net_attach(HvMachine *m, HvVirtioNet *n) {
    (void)m;
    (void)n;
    fprintf(stderr, "henge: built without libslirp: the guest has no network, and no shared folder\n");
    return -1;
}

void hv_net_poll(void) {}
