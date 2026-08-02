/*
 * ST7789 172x320 over SPI, via ESP-IDF's esp_lcd. Waveshare ESP32-C6-LCD-1.47.
 *
 * NO FULL FRAMEBUFFER. 172*320*2 = 110 KB, and this board has 512 KB of HP SRAM
 * that must also hold Forth, WiFi and eventually TLS. Everything draws through
 * small stack buffers blitted straight to the panel.
 */
#include <string.h>
#include "magnet_ui.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "magnet_ui";

/* Pinout from docs.waveshare.com/ESP32-C6-LCD-1.47 */
#define PIN_MOSI  6
#define PIN_SCLK  7
#define PIN_CS   14
#define PIN_DC   15
#define PIN_RST  21
#define PIN_BL   22

/* The ST7789 controller is 240x320; this panel is 172 wide and sits centred in
 * the controller's column space, so every write needs a 34-column offset.
 * Without it the image is shifted and the right edge wraps. */
#define X_GAP  34
#define Y_GAP   0

#define LCD_HOST      SPI2_HOST
#define LCD_HZ        (40 * 1000 * 1000)
#define LCD_CMD_BITS  8
#define LCD_PARAM_BITS 8

static esp_lcd_panel_handle_t s_panel;
static bool s_ready;

uint16_t ui_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    /* esp_lcd sends the buffer little-endian; this panel wants big-endian
     * pixels, so swap once here rather than per-pixel at every call site. */
    return (uint16_t)((c >> 8) | (c << 8));
}

void ui_backlight(int pct) {
    if (pct < 0) pct = 0;
    /* Hard clamp, not a suggestion. Waveshare: sustained full brightness
     * overheats the panel and leaves permanent dark shadows, and this device is
     * meant to sit displaying status indefinitely. */
    if (pct > UI_BL_MAX) pct = UI_BL_MAX;
    uint32_t duty = (uint32_t)((255 * pct) / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void backlight_init(void) {
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&c);
}

esp_err_t ui_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* Largest single blit we ever issue: one scaled glyph row-block. */
        .max_transfer_sz = UI_W * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = LCD_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                             &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        /* IDF 5.1 spells this rgb_endian; 5.2 renamed it rgb_ele_order with
         * LCD_RGB_ELEMENT_ORDER_*. Using the 5.2 name here compiles nowhere on
         * 5.1, which is the version installed. */
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* These IPS panels ship inverted; without this every colour is its
     * complement and the "dark" background comes out white. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, X_GAP, Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    backlight_init();
    s_ready = true;
    ui_clear(UI_BG);
    ui_backlight(40);           /* comfortably under the thermal ceiling */
    ESP_LOGI(TAG, "ST7789 %dx%d up (gap %d,%d)", UI_W, UI_H, X_GAP, Y_GAP);
    return ESP_OK;
}

void ui_set_mirror(bool mx, bool my) {
    if (s_panel) esp_lcd_panel_mirror(s_panel, mx, my);
}

void ui_set_swap_xy(bool swap) {
    if (s_panel) esp_lcd_panel_swap_xy(s_panel, swap);
}

void ui_fill(int x, int y, int w, int h, uint16_t colour) {
    if (!s_ready || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > UI_W || y + h > UI_H) return;
    /* Paint in row bands so the scratch buffer stays small regardless of the
     * rectangle: a full-screen clear would otherwise want all 110 KB. */
    enum { BAND = 16 };
    static uint16_t buf[UI_W * BAND];
    int n = (w > UI_W ? UI_W : w) * BAND;
    for (int i = 0; i < n; ++i) buf[i] = colour;
    for (int yy = y; yy < y + h; yy += BAND) {
        int rows = (y + h - yy) < BAND ? (y + h - yy) : BAND;
        esp_lcd_panel_draw_bitmap(s_panel, x, yy, x + w, yy + rows, buf);
    }
}

void ui_clear(uint16_t colour) { ui_fill(0, 0, UI_W, UI_H, colour); }

extern const uint8_t font5x7[59][5];

int ui_text_width(const char *s, int scale) {
    return (int)strlen(s) * 6 * scale;      /* 5 columns + 1 space */
}

void ui_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale) {
    if (!s_ready || scale < 1) return;
    const int gw = 6 * scale, gh = 8 * scale;
    /* One glyph at a time: at scale 4 that is 24x32 px = 1536 B, versus a
     * whole-line buffer that grows with the string. */
    uint16_t cell[6 * 4 * 8 * 4];
    if (gw * gh > (int)(sizeof(cell) / sizeof(cell[0]))) return;

    for (const char *p = s; *p; ++p, x += gw) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        int idx = (c < 32 || c > 90) ? 0 : (c - 32);
        if (x + gw > UI_W) break;
        for (int i = 0; i < gw * gh; ++i) cell[i] = bg;
        for (int col = 0; col < 5; ++col) {
            uint8_t bits = font5x7[idx][col];
            for (int row = 0; row < 7; ++row) {
                if (!(bits & (1 << row))) continue;
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale; ++sx)
                        cell[(row * scale + sy) * gw + (col * scale + sx)] = fg;
            }
        }
        if (y + gh <= UI_H) esp_lcd_panel_draw_bitmap(s_panel, x, y, x + gw, y + gh, cell);
    }
}

/*
 * Test card. Everything here is deliberately ASYMMETRIC so a single photograph
 * is decisive:
 *
 *   corner squares  R top-left, G top-right, B bottom-left, W bottom-right
 *                   -> names the mirror axis AND proves the RGB channel order
 *   1px white border-> proves the 34-column gap; a wrong gap loses an edge or
 *                      wraps one round
 *   big "F"         -> the classic orientation glyph: mirrored, rotated and
 *                      upside-down F are all instantly distinguishable, which
 *                      is not true of a symmetric shape
 *   "TOP" under it  -> settles 180-degree rotation, which the F alone leaves open
 */
void ui_testcard(void) {
    const int M = 24;
    ui_clear(ui_rgb565(0, 0, 0));

    /* 1px border in white */
    ui_fill(0, 0, UI_W, 1, ui_rgb565(0xFF, 0xFF, 0xFF));
    ui_fill(0, UI_H - 1, UI_W, 1, ui_rgb565(0xFF, 0xFF, 0xFF));
    ui_fill(0, 0, 1, UI_H, ui_rgb565(0xFF, 0xFF, 0xFF));
    ui_fill(UI_W - 1, 0, 1, UI_H, ui_rgb565(0xFF, 0xFF, 0xFF));

    ui_fill(2,          2,          M, M, ui_rgb565(0xFF, 0, 0));      /* R TL */
    ui_fill(UI_W-M-2,   2,          M, M, ui_rgb565(0, 0xFF, 0));      /* G TR */
    ui_fill(2,          UI_H-M-2,   M, M, ui_rgb565(0, 0, 0xFF));      /* B BL */
    ui_fill(UI_W-M-2,   UI_H-M-2,   M, M, ui_rgb565(0xFF,0xFF,0xFF));  /* W BR */

    ui_text(8, 34, "TOP", ui_rgb565(0xFF,0xFF,0xFF), ui_rgb565(0,0,0), 2);
    ui_text(30, 120, "F", ui_rgb565(0xFF,0xFF,0xFF), ui_rgb565(0,0,0), 12);
    ui_text(8, 250, "R-TL G-TR", ui_rgb565(0xFF,0xFF,0xFF), ui_rgb565(0,0,0), 1);
    ui_text(8, 264, "B-BL W-BR", ui_rgb565(0xFF,0xFF,0xFF), ui_rgb565(0,0,0), 1);
}
