/* test_pkg — run magnet_pkg.c's checks on one package file, as a node would.
 *   test_pkg <pkg> <chip> <board> <image_format> <slot_size> <running a.b.c+d> [transfer_len]
 * Prints the first failing code (or OK) and exits 0 — the runner compares. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "magnet_pkg.h"

static unsigned char *g_data; static long g_len;
static int rd(void *ctx, uint32_t off, void *buf, size_t n) {
    (void)ctx;
    if ((long)(off + n + MN_PKG_HDR_LEN) > g_len) return -1;
    memcpy(buf, g_data + MN_PKG_HDR_LEN + off, n);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); g_len = ftell(f); fseek(f, 0, SEEK_SET);
    g_data = malloc(g_len); fread(g_data, 1, g_len, f); fclose(f);
    mn_pkg_self_t self = { (uint16_t)strtoul(argv[2], 0, 0), (uint16_t)strtoul(argv[3], 0, 0),
                           (uint8_t)atoi(argv[4]), (uint32_t)strtoul(argv[5], 0, 0) };
    unsigned a, b, c, d; sscanf(argv[6], "%u.%u.%u+%u", &a, &b, &c, &d);
    self.running = (mn_pkg_ver_t){ (uint8_t)a, (uint8_t)b, (uint16_t)c, d };
    uint32_t transfer = argc > 7 ? (uint32_t)strtoul(argv[7], 0, 0) : (uint32_t)g_len;
    mn_pkg_hdr_t h;
    int rc = g_len >= MN_PKG_HDR_LEN ? mn_pkg_check_header(g_data, transfer, &self, &h) : MN_PKG_E_FORMAT;
    if (rc == MN_PKG_OK) rc = mn_pkg_check_payload(&h, rd, NULL);
    if (rc == MN_PKG_OK) rc = mn_pkg_check_min_running(&h, &self);
    printf("%s\n", mn_pkg_rc_name(rc));
    return 0;
}
