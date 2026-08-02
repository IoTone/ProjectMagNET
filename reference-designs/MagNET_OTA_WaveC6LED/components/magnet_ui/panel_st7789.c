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
#include "Vernon_ST7789T.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "magnet_ui";

/* Pinout from docs.waveshare.com/ESP32-C6-LCD-1.47 */
#define PIN_MOSI  6
#define PIN_SCLK  7
#define PIN_CS   14
#define PIN_DC   15
#define PIN_RST  21
#define PIN_BL   22

/* Glass is 172 columns centred in the controller's 240, so writes need a
 * 34-column offset. */
#define X_GAP  34
#define Y_GAP   0

#define LCD_HOST      SPI2_HOST
/* 12 MHz, matching the vendor demo. 40 MHz appeared to work and is not
 * worth the risk on a panel whose init sequence we are already trusting
 * them for. */
#define LCD_HZ        (12 * 1000 * 1000)
#define LCD_CMD_BITS  8
#define LCD_PARAM_BITS 8

static esp_lcd_panel_handle_t s_panel;
static bool s_ready;

/*
 * esp_lcd_panel_io_tx_color() is ASYNCHRONOUS — it queues the SPI transfer and
 * returns immediately. Every blit here reuses one static scratch buffer, so
 * without waiting for completion the next glyph overwrites pixels that have not
 * been sent yet and the panel receives a mixture of two glyphs.
 *
 * That failure mode is deceptive: ui_fill() is immune BY ACCIDENT, because every
 * band it sends contains the same colour, so clobbering the buffer mid-transfer
 * writes identical bytes. Solid shapes therefore looked perfect while text
 * shattered into garbage — which reads as "the font is broken" and sends you off
 * to audit the glyph table, the renderer and the panel orientation, none of
 * which were wrong.
 */
static SemaphoreHandle_t s_blit_done;

static bool IRAM_ATTR on_colour_done(esp_lcd_panel_io_handle_t io,
                                     esp_lcd_panel_io_event_data_t *ev,
                                     void *ctx) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_blit_done, &hp);
    return hp == pdTRUE;
}

/* Blit and WAIT. Every panel write in this file goes through here so no caller
 * can forget. */
static void blit(int x0, int y0, int x1, int y1, const void *px) {
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, px);
    xSemaphoreTake(s_blit_done, pdMS_TO_TICKS(200));
}

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
        .on_color_trans_done = on_colour_done,
    };
    s_blit_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                             &io_cfg, &io));

    /* Waveshare's own ESP-IDF demo does NOT use esp_lcd's generic ST7789 —
     * it ships this ST7789T driver, because the panel needs a different init
     * sequence (its own porch, power and gamma tables, plus INVON). Chasing
     * MADCTL bits against the generic driver was never going to converge; the
     * vendor's init is the ground truth and is vendored here beside us. */
    esp_lcd_panel_dev_st7789t_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        /* BGR, not RGB. From the vendor demo — with RGB the red and blue
         * channels swap. */
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789t(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* Inversion is already in the vendor init sequence (0x21), so do NOT call
     * invert_color here — doing both cancels out. */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
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
        blit(x, yy, x + w, yy + rows, buf);
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

    /* Band the glyph by ROWS, exactly as ui_fill does, so the scratch buffer is
     * bounded by width alone and ANY scale renders.
     *
     * The previous version used one fixed 768-pixel cell and simply `return`ed
     * when a glyph did not fit. At scale 12 a glyph needs 6912, so the big
     * orientation "F" on the test card silently never drew — and it was the one
     * element whose whole job was to be unmissable. A guard that quietly drops
     * output is worse than no guard: it cost a flash cycle and a photograph to
     * notice something was absent rather than wrong. */
    enum { BAND = 16 };
    static uint16_t band[UI_W * BAND];
    if (gw > UI_W) return;

    for (const char *p = s; *p; ++p, x += gw) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        int idx = (c < 32 || c > 90) ? 0 : (c - 32);
        if (x < 0 || x + gw > UI_W) break;

        for (int y0 = 0; y0 < gh; y0 += BAND) {
            int rows = (gh - y0) < BAND ? (gh - y0) : BAND;
            if (y + y0 + rows > UI_H) break;
            for (int i = 0; i < gw * rows; ++i) band[i] = bg;
            for (int col = 0; col < 5; ++col) {
                uint8_t bits = font5x7[idx][col];
                for (int row = 0; row < 7; ++row) {
                    if (!(bits & (1 << row))) continue;
                    for (int sy = 0; sy < scale; ++sy) {
                        int py = row * scale + sy - y0;
                        if (py < 0 || py >= rows) continue;
                        for (int sx = 0; sx < scale; ++sx)
                            band[py * gw + (col * scale + sx)] = fg;
                    }
                }
            }
            blit(x, y + y0, x + gw, y + y0 + rows, band);
        }
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
