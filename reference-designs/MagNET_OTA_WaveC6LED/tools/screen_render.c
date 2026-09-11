/*
 * screen_render — the status screen, rendered on the host.
 *
 *   cc -I../components/magnet_ui -o screen_render screen_render.c ../components/magnet_ui/font5x7.c
 *   ./screen_render out.ppm [STATE] [checkin-status] [net1] [net2] [heapKB] [xshift]
 *   sips -s format png out.ppm --out out.png
 *
 * WHY: the board has no framebuffer to dump (172*320*2 B would be a fifth of
 * its RAM), so "take a screenshot" cannot be answered from the device. This
 * reproduces what main.c's draw_status() sends — same font, same primitives,
 * same coordinates — into a PPM, so a layout can be reviewed without a flash
 * cycle and a photograph. It found the CHECK-IN / FREE RAM overlap that had
 * shipped unnoticed since those rows were added in different phases.
 *
 * XSHIFT simulates a panel gap error: the touch unit with x_gap 0 showed the
 * image shifted 34 px left. Pass 34 to see what the owner saw.
 *
 * DUPLICATION, ON PURPOSE: draw_status() is copied from main.c rather than
 * linked, because main.c is inseparable from ESP-IDF. If the layout changes
 * there, change it here — this file is the review tool for exactly that edit.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define UI_W 172
#define UI_H 320
extern const uint8_t font5x7[59][5];

static uint8_t fb[UI_H][UI_W][3];
static int xshift;

typedef struct { uint8_t r, g, b; } rgb;
#define C(r,g,b) ((rgb){r,g,b})
static const rgb UI_BG = C(0x0A,0x0A,0x12), UI_CYAN = C(0x36,0xB5,0xFF), UI_AMBER = C(0xFF,0x9E,0x2A),
                 UI_GREEN = C(0x5A,0xFF,0x8A), UI_RED = C(0xFF,0x4D,0x5A), UI_WHITE = C(0xE8,0xF0,0xFF),
                 UI_DIM = C(0x4A,0x52,0x60);

static void put(int x, int y, rgb c) {
    x -= xshift;
    if (x < 0 || y < 0 || x >= UI_W || y >= UI_H) return;
    fb[y][x][0] = c.r; fb[y][x][1] = c.g; fb[y][x][2] = c.b;
}
static void ui_fill(int x, int y, int w, int h, rgb c) {
    if (w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > UI_W || y + h > UI_H) return;   /* same guard as the firmware */
    for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) put(xx, yy, c);
}
static void ui_clear(rgb c) { for (int y = 0; y < UI_H; ++y) for (int x = 0; x < UI_W; ++x) put(x + xshift, y, c); }
static void ui_text(int x, int y, const char *s, rgb fg, rgb bg, int scale) {
    const int gw = 6 * scale, gh = 8 * scale;
    for (const char *p = s; *p; ++p, x += gw) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        int idx = (c < 32 || c > 90) ? 0 : (c - 32);
        if (x < 0 || x + gw > UI_W) break;                     /* firmware clips whole glyphs */
        for (int yy = 0; yy < gh; ++yy) for (int xx = 0; xx < gw; ++xx) put(x + xx, y + yy, bg);
        for (int col = 0; col < 5; ++col) {
            uint8_t bits = font5x7[idx][col];
            for (int row = 0; row < 7; ++row) {
                if (!(bits & (1 << row))) continue;
                for (int sy = 0; sy < scale; ++sy) for (int sx = 0; sx < scale; ++sx)
                    put(x + col * scale + sx, y + row * scale + sy, fg);
            }
        }
    }
}

/* === copy of main.c draw_status(), inputs parameterised === */
static void draw_status(const char *board, const char *fw, const char *state, rgb state_colour,
                        const char *checkin, unsigned long heap_kb, const char *net1, rgb net1c, const char *net2) {
    char line[32];
    ui_clear(UI_BG);
    ui_fill(0, 0, UI_W, 3, UI_CYAN);
    ui_text(6, 12, "ROBOTARME", UI_CYAN, UI_BG, 2);
    ui_text(6, 32, "OTA CLIENT", UI_DIM, UI_BG, 1);
    ui_fill(6, 48, UI_W - 12, 1, UI_DIM);
    ui_text(6, 60, "DEVICE", UI_DIM, UI_BG, 1);
    ui_text(6, 74, board, UI_WHITE, UI_BG, 2);
    ui_text(6, 104, "FIRMWARE", UI_DIM, UI_BG, 1);
    snprintf(line, sizeof line, "E4TH %s", fw);
    ui_text(6, 118, line, UI_AMBER, UI_BG, 2);
    ui_text(6, 148, "STATE", UI_DIM, UI_BG, 1);
    ui_text(6, 162, state, state_colour, UI_BG, 2);
    ui_text(6, 190, "CHECK-IN", UI_DIM, UI_BG, 1);
    ui_text(6, 204, checkin, UI_CYAN, UI_BG, 1);
    ui_text(6, 228, "FREE RAM", UI_DIM, UI_BG, 1);
    snprintf(line, sizeof line, "%lu KB", heap_kb);
    ui_text(6, 242, line, UI_WHITE, UI_BG, 2);
    ui_text(6, 270, "NETWORK", UI_DIM, UI_BG, 1);
    ui_text(6, 284, net1, net1c, UI_BG, 1);
    ui_text(6, 298, net2, UI_WHITE, UI_BG, 1);
    ui_fill(0, UI_H - 3, UI_W, 3, UI_CYAN);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s out.ppm [STATE] [checkin] [net1] [net2] [heapKB] [xshift]\n", argv[0]); return 2; }
    const char *state = argc > 2 ? argv[2] : "ONLINE";
    const char *chk   = argc > 3 ? argv[3] : "up to date";
    const char *net1  = argc > 4 ? argv[4] : "BLE RELAY";
    const char *net2  = argc > 5 ? argv[5] : "ROBOTARME-7756";
    unsigned long kb  = argc > 6 ? strtoul(argv[6], NULL, 10) : 76;
    xshift            = argc > 7 ? atoi(argv[7]) : 0;
    rgb sc = strcmp(state, "ONLINE") == 0 || strcmp(state, "APPLIED") == 0 ? UI_GREEN
           : strstr(state, "FAIL") || strstr(state, "ERR") ? UI_RED : UI_AMBER;
    memset(fb, 0, sizeof fb);   /* off-glass columns after a shift stay black, as on the panel */
    draw_status("WAVEC6TOUCH", "0.5.0", state, sc, chk, kb, net1, UI_GREEN, net2);
    FILE *f = fopen(argv[1], "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
    fwrite(fb, 1, sizeof fb, f);
    fclose(f);
    return 0;
}
