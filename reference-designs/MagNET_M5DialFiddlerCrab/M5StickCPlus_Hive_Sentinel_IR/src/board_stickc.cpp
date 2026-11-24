// board_stickc — M5StickC Plus board layer for the Sentinel IR. See board_stickc.h.

#include <M5GFX.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "board_stickc.h"

static M5GFX display;

#define AXP192_ADDR      0x34
#define AXP192_I2C_PORT  1
#define AXP192_I2C_HZ    400000

#define C_BG      0x0841   // near-black blue
#define C_FG      0xFFFF
#define C_DIM     0x8410
#define C_DOME    0xBDF7   // milky lens
#define C_DOME_D  0x7BEF
#define C_HOT     0xFA20   // orange-red: motion
#define C_LIVE    0xF800
#define C_OK      0x07E0   // green
#define C_WARN    0xFFE0   // yellow
#define C_BAR     0x2945   // legend bars
#define C_PCB     0x0320   // dark green

// ---------------------------------------------------------------- power

static void axp192_adc_enable(void) {
    // Reg 0x82 bit 7: battery-voltage ADC enable.
    auto v = lgfx::i2c::readRegister8(AXP192_I2C_PORT, AXP192_ADDR, 0x82, AXP192_I2C_HZ);
    if (v.has_value()) {
        lgfx::i2c::writeRegister8(AXP192_I2C_PORT, AXP192_ADDR, 0x82,
                                  v.value() | 0x80, 0xFF, AXP192_I2C_HZ);
    }
}

extern "C" void board_batt_read(board_batt_t *out) {
    out->mv = -1; out->pct = -1; out->dischg_ma = 0; out->charging = false; out->usb = false;
    uint8_t b[4] = {0};
    // 0x78-0x79 voltage (12-bit, 1.1 mV/LSB); 0x7A-0x7B discharge (13-bit, 0.5 mA/LSB)
    auto r = lgfx::i2c::readRegister(AXP192_I2C_PORT, AXP192_ADDR, 0x78, b, 4, AXP192_I2C_HZ);
    if (!r.has_value()) return;
    uint16_t v_raw = ((uint16_t)b[0] << 4) | (b[1] & 0x0F);
    uint16_t i_raw = ((uint16_t)b[2] << 5) | (b[3] & 0x1F);
    out->mv        = (int)(v_raw * 1.1f);
    out->dischg_ma = (int)(i_raw * 0.5f);
    int pct = (out->mv - 3300) * 100 / (4150 - 3300);
    out->pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    // 0x00: bit 5 VBUS present, bit 2 battery charging.
    auto st = lgfx::i2c::readRegister8(AXP192_I2C_PORT, AXP192_ADDR, 0x00, AXP192_I2C_HZ);
    out->charging = st.has_value() && (st.value() & 0x04);
    out->usb      = st.has_value() && (st.value() & 0x20);
}

// Power key (PEK, bottom edge). Same setup as the Ear: 0x36 long-press
// 1.5 s, hardware off after a 6 s hold as a backstop; 0x42 bits 1/0 enable
// the short/long IRQ flags latched in 0x46 (write 1 to clear); 0x32 bit 7 =
// power off.
static void axp192_pek_init(void) {
    auto v = lgfx::i2c::readRegister8(AXP192_I2C_PORT, AXP192_ADDR, 0x36, AXP192_I2C_HZ);
    if (v.has_value())
        lgfx::i2c::writeRegister8(AXP192_I2C_PORT, AXP192_ADDR, 0x36,
                                  (v.value() & 0xC4) | 0x10 | 0x08 | 0x01, 0x00, AXP192_I2C_HZ);
    lgfx::i2c::bitOn(AXP192_I2C_PORT, AXP192_ADDR, 0x42, 0x03, AXP192_I2C_HZ);
    lgfx::i2c::writeRegister8(AXP192_I2C_PORT, AXP192_ADDR, 0x46, 0x03, 0x00, AXP192_I2C_HZ);
}

extern "C" int board_power_key(void) {
    auto v = lgfx::i2c::readRegister8(AXP192_I2C_PORT, AXP192_ADDR, 0x46, AXP192_I2C_HZ);
    if (!v.has_value() || !(v.value() & 0x03)) return 0;
    lgfx::i2c::writeRegister8(AXP192_I2C_PORT, AXP192_ADDR, 0x46, v.value() & 0x03, 0x00, AXP192_I2C_HZ);
    return (v.value() & 0x01) ? 2 : 1;
}

extern "C" void board_power_off(void) {
    lgfx::i2c::bitOn(AXP192_I2C_PORT, AXP192_ADDR, 0x32, 0x80, AXP192_I2C_HZ);
}

extern "C" int board_axp_read(int reg) {
    auto v = lgfx::i2c::readRegister8(AXP192_I2C_PORT, AXP192_ADDR, (uint8_t)reg, AXP192_I2C_HZ);
    return v.has_value() ? v.value() : -1;
}

// ---------------------------------------------------------------- buttons / LED / PIR

typedef struct { gpio_num_t pin; bool state; bool raw; int64_t since; } btn_t;
static btn_t s_a = { (gpio_num_t)BOARD_BTN_A_GPIO, false, false, 0 };
static btn_t s_b = { (gpio_num_t)BOARD_BTN_B_GPIO, false, false, 0 };
#define DEBOUNCE_US 20000

static void btn_poll(btn_t *b) {
    bool raw = gpio_get_level(b->pin) == 0;           // active-low
    int64_t now = esp_timer_get_time();
    if (raw != b->raw) { b->raw = raw; b->since = now; }
    else if (raw != b->state && now - b->since >= DEBOUNCE_US) b->state = raw;
}

extern "C" void board_buttons_poll(void) { btn_poll(&s_a); btn_poll(&s_b); }
extern "C" bool board_btn_a_down(void)   { return s_a.state; }
extern "C" bool board_btn_b_down(void)   { return s_b.state; }
extern "C" bool board_pir_raw(void)      { return gpio_get_level((gpio_num_t)BOARD_PIR_GPIO) == 1; }

extern "C" void board_led(bool on) { gpio_set_level((gpio_num_t)BOARD_LED_GPIO, on ? 0 : 1); }

// ---------------------------------------------------------------- display

extern "C" void board_brightness(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    display.setBrightness((pct * 255) / 100);
}

// The PIR's Fresnel dome on its little PCB; hot + waves while it sees motion.
static void draw_dome(int cx, int cy, bool motion) {
    display.fillCircle(cx, cy, 18, motion ? C_HOT : C_DOME);
    display.fillRect(cx - 19, cy + 1, 39, 19, C_BG);            // keep the top half
    display.fillRect(cx - 24, cy + 1, 48, 6, C_PCB);
    for (int dx = -12; dx <= 12; dx += 6)                       // Fresnel facets
        display.drawLine(cx + dx, cy, cx + dx / 2, cy - 16, motion ? C_LIVE : C_DOME_D);
    if (motion)
        for (int r = 24; r <= 32; r += 8)
            display.drawArc(cx, cy, r, r - 1, 200, 340, C_HOT);
}

// Legend text: size 2 when it fits, else size 1.
static void legend(const char *t, int x0, int x1, int y, uint16_t bg) {
    int sz = (int)strlen(t) * 12 <= x1 - x0 ? 2 : 1;
    display.setTextSize(sz);
    display.setTextColor(C_FG, bg);
    int w = display.textWidth(t);
    display.setCursor(x0 + (x1 - x0 - w) / 2, y + (sz == 2 ? 2 : 6));
    display.print(t);
}

extern "C" void board_draw(const board_screen_t *s) {
    display.startWrite();
    display.fillScreen(C_BG);
    if (s->icon) draw_dome(30, 64, s->motion);

    // B lives on the top edge: bar along the top, arrows pointing up at it.
    if (s->btn_b && *s->btn_b) {
        display.fillRect(0, 0, 240, 20, C_BAR);
        display.fillTriangle(4, 17, 16, 17, 10, 3, C_WARN);
        display.fillTriangle(224, 17, 236, 17, 230, 3, C_WARN);
        legend(s->btn_b, 20, 220, 0, C_BAR);
    }

    // A is the front button right of the screen: bottom bar, arrow right.
    if (s->btn_a && *s->btn_a) {
        display.fillRect(0, 115, 240, 20, C_BAR);
        display.fillTriangle(222, 117, 222, 133, 237, 125, C_OK);
        legend(s->btn_a, 0, 218, 115, C_BAR);
    }

    static const uint16_t tone[] = { C_FG, C_OK, C_WARN, C_LIVE };
    int x = s->icon ? 64 : 8;
    int y = 24;
    if (s->title && *s->title) {
        display.setTextColor(tone[s->tone], C_BG);
        display.setTextSize(s->title_big ? 3 : 2);
        display.setCursor(x, y);
        display.print(s->title);
        y += s->title_big ? 28 : 20;
    }

    int sz = s->lines_big ? 2 : 1;
    display.setTextSize(sz);
    display.setTextColor(C_FG, C_BG);
    for (int i = 0; i < BOARD_LINES; i++) {
        if (!s->lines[i] || !*s->lines[i]) continue;
        display.setCursor(x, y);
        display.print(s->lines[i]);
        y += sz == 2 ? 18 : 12;
    }
    display.endWrite();
}

// ---------------------------------------------------------------- init

extern "C" void board_init(void) {
    // M5GFX auto-detects the StickC Plus and brings up I2C port 1 + the
    // AXP192 LDOs for the panel/backlight. Must precede any lgfx::i2c use.
    display.init();
    display.setRotation(1);          // 240x135 landscape, USB on the right
    display.setTextWrap(false);
    board_brightness(60);
    axp192_adc_enable();
    axp192_pek_init();

    // Buttons: GPIO37/39 are input-only with NO internal pulls — the board
    // has external pull-ups.
    gpio_config_t in = {};
    in.mode = GPIO_MODE_INPUT;
    in.pin_bit_mask = (1ULL << BOARD_BTN_A_GPIO) | (1ULL << BOARD_BTN_B_GPIO);
    gpio_config(&in);

    // PIR OUT drives high on motion. The pull-down holds the line low when
    // the sensor is unplugged, so a loose wire reads "no motion" instead of
    // a stream of phantom room entries.
    gpio_config_t pir = {};
    pir.mode = GPIO_MODE_INPUT;
    pir.pin_bit_mask = 1ULL << BOARD_PIR_GPIO;
    pir.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&pir);

    gpio_config_t led = {};
    led.mode = GPIO_MODE_OUTPUT;
    led.pin_bit_mask = 1ULL << BOARD_LED_GPIO;
    gpio_config(&led);
    board_led(false);
}
