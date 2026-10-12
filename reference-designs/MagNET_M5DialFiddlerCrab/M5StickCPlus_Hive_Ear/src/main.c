/*
 * M5StickCPlus_Hive_Ear — Role 13 (Ear), BLE-only hive accessory.
 *
 * "An ear has the capacity to hear audible sounds and capture them, and
 *  pass along to interested parties. It has a special skill of sharing
 *  audio streams." — README design section.
 *
 * Hardware: M5StickC Plus (original, AXP192). ESP32-PICO-D4, no PSRAM,
 * SPM1423 PDM mic (CLK G0 / DAT G34), ST7789V2 135x240, CH552 UART console.
 *
 * Accessory model (docs/PLAN.md D7): the Ear has NO WiFi and does not join
 * the hive itself. It is a plain, standards-only BLE peripheral; its bonded
 * consumer (the Genio 720) reads DIS/BAS/ATVV over the link and publishes
 * the Ear's hive record on its behalf. All radio time and RAM go to BLE audio.
 *
 * E0 scope (this file today):
 *   - NimBLE peripheral: DIS + BAS, LE SC bonding with passkey on screen.
 *   - Memory instrumentation at each bringup milestone (`ear-mem`).
 *   - Display / battery / buttons / LED.
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_bt.h"
#include "driver/uart.h"
#include "nvs_flash.h"

#include "forth_core.h"
#include "forth_version.h"
#include "board_stickc.h"
#include "ear_ble.h"
#include "ear_audio.h"
#include "ear_atvv.h"
#include "ima_adpcm.h"
#include "nvs.h"
#include <math.h>
#include "../../include/magnet_gen.h"

static const char *TAG = "ear";

#ifndef EAR_DEV_JUSTWORKS
#define EAR_DEV_JUSTWORKS 0
#endif

#define EAR_FW_VERSION   "0.4.0-e2"
#define EAR_MODEL        "MagNET Ear (M5StickC Plus)"
#define FORTH_HEAP_SIZE  (32 * 1024)
#define BTN_B_UNPAIR_MS  5000      /* hold side button → forget all bonds */
#define BTN_B_SHORT_MS   1000      /* shorter press → toggle pairing window */
#define PAIR_WINDOW_S    120

/* ---------- Console (UART0 via CH552) ---------- */
#define CONSOLE_UART UART_NUM_0
static void console_init(void) {
    uart_config_t cfg = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(CONSOLE_UART, 512, 512, 0, NULL, 0);
    uart_param_config(CONSOLE_UART, &cfg);
}
static void uprint(const char *s) { uart_write_bytes(CONSOLE_UART, s, strlen(s)); }
static void uprintf(const char *fmt, ...) {
    char buf[256]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    uprint(buf);
}
static int console_getchar(void) {
    uint8_t c; int n = uart_read_bytes(CONSOLE_UART, &c, 1, pdMS_TO_TICKS(10));
    return n > 0 ? c : -1;
}
static void console_putchar(int c) {
    uint8_t ch = (uint8_t)c;
    uart_write_bytes(CONSOLE_UART, &ch, 1);
}

/* ---------- Memory instrumentation (E0 gate) ---------- */
typedef struct { size_t free, largest, min_ever; } mem_snap_t;
static mem_snap_t mem_snap(void) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    return (mem_snap_t){
        heap_caps_get_free_size(caps),
        heap_caps_get_largest_free_block(caps),
        heap_caps_get_minimum_free_size(caps),
    };
}
static void mem_report(const char *label) {
    mem_snap_t m = mem_snap();
    uprintf("[MEM] %-14s free=%6u largest=%6u min_ever=%6u\r\n",
            label, (unsigned)m.free, (unsigned)m.largest, (unsigned)m.min_ever);
}

/* ---------- Identity ---------- */
static char node_id[40] = "MagNET-biologic-0000";   /* DIS serial = hive node id */

static void derive_ids(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(node_id, sizeof(node_id), "MagNET-biologic-%02x%02x", mac[4], mac[5]);
}

/* ---------- BLE events → UI ---------- */
static volatile uint32_t s_passkey;          /* shown while pairing */
static volatile int64_t  s_passkey_until;
static volatile int64_t  s_refused_until;    /* flash "press B first" */
static volatile int64_t  s_paired_until;     /* flash "PAIRED" after a bond */

static void on_ble(ear_ble_evt_t evt, uint32_t arg) {
    switch (evt) {
    case EAR_BLE_EVT_PASSKEY:
        s_passkey = arg;
        s_passkey_until = esp_timer_get_time() + 60 * 1000000LL;
        uprintf("\r\n[BLE] pairing — passkey %06lu\r\n", (unsigned long)arg);
        break;
    case EAR_BLE_EVT_PAIRED:
        if (arg && s_passkey_until) s_paired_until = esp_timer_get_time() + 4 * 1000000LL;
        s_passkey_until = 0;
        uprintf("\r\n[BLE] %s\r\n", arg ? "bonded + encrypted" : "pairing FAILED");
        mem_report("paired");
        break;
    case EAR_BLE_EVT_CONNECTED:
        uprintf("\r\n[BLE] connected (%lu)\r\n", (unsigned long)arg);
        mem_report("connected");
        break;
    case EAR_BLE_EVT_DISCONNECTED:
        s_passkey_until = 0;
        uprintf("\r\n[BLE] disconnected (%lu)\r\n", (unsigned long)arg);
        break;
    case EAR_BLE_EVT_PAIR_REFUSED:
        s_refused_until = esp_timer_get_time() + 5 * 1000000LL;
        uprint("\r\n[BLE] pairing refused — press B on the Ear first\r\n");
        break;
    }
}

/* ---------- UI: buttons + screen + battery ---------- */
/* What the screen says, and what the footer offers, for each state:
 *
 * Legends are drawn beside the physical button (top bar = B, arrow up;
 * bottom-right = A, arrow right):
 *   B held >1 s        FORGET ALL? n    B: release: cancel
 *   passkey shown      123456           B: cancel
 *   streaming          TALKING / OPEN   A: release A: stop (HTT only)
 *   ready              READY            A: hold A: talk
 *   pairing window     PAIRING m:ss     B: cancel
 *   not paired         NOT PAIRED       B: pair
 *   paired, no host    PAIRED           B: pair / hold 5s: forget
 */
static void ui_task(void *arg) {
    (void)arg;
    char ban[24], l1[48], l2[48], l3[48], la[32], lb[32], prev[256] = "";
    int64_t b_down_since = 0, last_draw = 0, last_batt = -60000000;
    bool a_prev = false;
    board_batt_t batt = { .pct = -1 };
    while (1) {
        board_buttons_poll();
        int64_t now = esp_timer_get_time();

        /* Button A = hold-to-talk (ATVV HTT). Edges only. */
        bool a = board_btn_a_down();
        if (a != a_prev) { ear_atvv_button(a); a_prev = a; }
        atvv_status_t at; ear_atvv_status(&at);
        board_led(at.state == ATVV_LIVE);

        /* Button B: short press toggles the pairing window, 5 s forgets bonds. */
        int b_held_ms = 0;
        if (board_btn_b_down()) {
            if (!b_down_since) b_down_since = now;
            b_held_ms = (int)((now - b_down_since) / 1000);
            if (b_held_ms > BTN_B_UNPAIR_MS) {
                uprint("\r\n[UI] B held — forgetting all bonds, rebooting\r\n");
                board_screen_t s = { "FORGOTTEN", BOARD_TONE_ALERT, false,
                                     "all pairings cleared", "rebooting...", "", "", "", false };
                board_draw(&s);
                ear_ble_unpair_all();
                vTaskDelay(pdMS_TO_TICKS(800));
                esp_restart();
            }
        } else if (b_down_since) {
            int held = (int)((now - b_down_since) / 1000);
            b_down_since = 0;
            if (held < BTN_B_SHORT_MS) {
                bool open = ear_ble_pairing_left_s() == 0;
                ear_ble_pairing(open, PAIR_WINDOW_S);
                if (!open) s_passkey_until = 0;
                uprintf("\r\n[UI] pairing window %s\r\n", open ? "OPEN (120 s)" : "closed");
            }
        }

        int pair_left = ear_ble_pairing_left_s();
        if (pair_left == 0 && ear_ble_advertising() && ear_ble_bond_count() == 0 &&
            !ear_ble_conn_count())
            ear_ble_pairing(false, 0);           /* window expired: go invisible */

        /* Power key (bottom edge): long press = power off. */
        static int64_t last_pek;
        if (now - last_pek > 100000) {
            last_pek = now;
            if (board_power_key() == 2) {
                uprint("\r\n[UI] power key held — powering off\r\n");
                board_screen_t s = { "POWER OFF", BOARD_TONE_ALERT, false,
                                     "press the power key", "(bottom edge) to turn on", "", "", "", false };
                board_draw(&s);
                vTaskDelay(pdMS_TO_TICKS(800));
                board_power_off();
            }
        }

        if (now - last_batt > 30000000) {
            board_batt_read(&batt);
            if (batt.pct >= 0) ear_ble_battery_set((uint8_t)batt.pct);
            last_batt = now;
        }

        if (now - last_draw > 250000) {
            last_draw = now;
            board_tone_t tone = BOARD_TONE_NORMAL;
            bool big = false;
            int bonds = ear_ble_bond_count();
            ban[0] = l1[0] = l2[0] = la[0] = lb[0] = 0;
            snprintf(l1, sizeof(l1), "%s", ear_ble_name());

            if (b_held_ms >= BTN_B_SHORT_MS) {
                snprintf(ban, sizeof(ban), "FORGET ALL? %d",
                         (BTN_B_UNPAIR_MS - b_held_ms + 999) / 1000);
                tone = BOARD_TONE_ALERT;
                snprintf(l2, sizeof(l2), "keep holding B to clear");
                snprintf(lb, sizeof(lb), "release B: cancel");
            } else if (s_passkey_until > now) {
                snprintf(ban, sizeof(ban), "%06lu", (unsigned long)s_passkey);
                tone = BOARD_TONE_WARN; big = true;
                snprintf(l2, sizeof(l2), "type this code on the host");
                snprintf(lb, sizeof(lb), "B: cancel");
            } else if (s_paired_until > now) {
                snprintf(ban, sizeof(ban), "PAIRED");
                tone = BOARD_TONE_OK;
                snprintf(l2, sizeof(l2), "host bonded");
                snprintf(la, sizeof(la), "hold A: talk");
            } else if (at.state == ATVV_LIVE && at.waiting_host) {
                snprintf(ban, sizeof(ban), at.capturing ? "CALLING HOST" : "SENDING");
                tone = BOARD_TONE_WARN;
                snprintf(l2, sizeof(l2), "buffered %u.%u s, waiting",
                         at.buffered_ms / 1000, (at.buffered_ms % 1000) / 100);
                if (at.capturing) snprintf(la, sizeof(la), "release A: done");
            } else if (at.state == ATVV_LIVE) {
                bool htt = at.reason == 0x03;
                snprintf(ban, sizeof(ban), !at.capturing ? "SENDING" : htt ? "TALKING" : "MIC OPEN");
                tone = BOARD_TONE_ALERT;
                snprintf(l2, sizeof(l2), "sent %lu  queued %u ms",
                         (unsigned long)at.frames_sent, at.buffered_ms);
                if (htt && at.capturing) snprintf(la, sizeof(la), "release A: stop");
            } else if (at.no_host) {
                snprintf(ban, sizeof(ban), "NO HOST");
                tone = BOARD_TONE_ALERT;
                snprintf(l2, sizeof(l2), "is the host app running?");
                snprintf(la, sizeof(la), "hold A: try again");
            } else if (at.state == ATVV_READY) {
                snprintf(ban, sizeof(ban), "READY");
                tone = BOARD_TONE_OK;
                snprintf(l2, sizeof(l2), "host connected");
                snprintf(la, sizeof(la), "hold A: talk");
            } else if (pair_left) {
                snprintf(ban, sizeof(ban), "PAIRING %d:%02d", pair_left / 60, pair_left % 60);
                tone = BOARD_TONE_WARN;
                snprintf(l2, sizeof(l2), ear_ble_conn_count() ? "host connected, securing..."
                                                              : "connect from host now");
                snprintf(lb, sizeof(lb), "B: cancel");
            } else if (s_refused_until > now) {
                snprintf(ban, sizeof(ban), "PRESS B");
                tone = BOARD_TONE_ALERT;
                snprintf(l2, sizeof(l2), "new host refused");
                snprintf(lb, sizeof(lb), "B: pair");
            } else if (ear_ble_conn_count()) {
                snprintf(ban, sizeof(ban), "CONNECTED");
                snprintf(l2, sizeof(l2), ear_ble_encrypted() ? "bonded, waiting for host app"
                                                             : "not secured yet");
                snprintf(lb, sizeof(lb), bonds ? "B: pair another" : "B: pair");
                if (bonds) snprintf(la, sizeof(la), "hold A: talk");
            } else if (bonds == 0) {
                snprintf(ban, sizeof(ban), "NOT PAIRED");
                tone = BOARD_TONE_ALERT;
                snprintf(l2, sizeof(l2), "press B (side) to pair");
                snprintf(lb, sizeof(lb), "B: pair");
            } else {
                snprintf(ban, sizeof(ban), "PAIRED");
                snprintf(l2, sizeof(l2), "%d host%s, none connected", bonds, bonds == 1 ? "" : "s");
                snprintf(lb, sizeof(lb), "B: pair   hold 5s: forget");
                snprintf(la, sizeof(la), "hold A: talk");
            }

            mem_snap_t m = mem_snap();
#if EAR_DEV_JUSTWORKS
            snprintf(l3, sizeof(l3), "DEV: NO MITM  batt %d%%", batt.pct);
#else
            snprintf(l3, sizeof(l3), "batt %d%%  heap %uk", batt.pct, (unsigned)(m.free / 1024));
#endif
            char cur[256];
            snprintf(cur, sizeof(cur), "%s|%s|%s|%s|%s|%s|%d", ban, l1, l2, l3, la, lb, at.state);
            if (strcmp(cur, prev)) {
                strcpy(prev, cur);
                board_screen_t s = { ban, tone, big, l1, l2, l3, la, lb,
                                     at.state == ATVV_LIVE };
                board_draw(&s);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


/* ---------- E1: capture diagnostics ---------- */
#define EAR_NS        "ear"
#define EAR_KEY_GAIN  "gain"
#define REC_FRAME     128                    /* ADPCM bytes = 256 samples = 16 ms */
#define REC_MAX_SECS  5

static uint8_t *s_rec;                       /* ADPCM, 8000 B/s at 16 kHz */
static size_t   s_rec_len, s_rec_cap;
static volatile bool s_rec_full;

static void gain_load(void) {
    nvs_handle_t h; uint8_t g;
    if (nvs_open(EAR_NS, NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_u8(h, EAR_KEY_GAIN, &g) == ESP_OK) ear_audio_set_gain(g);
    nvs_close(h);
}
static void gain_save(int g) {
    nvs_handle_t h;
    if (nvs_open(EAR_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, EAR_KEY_GAIN, (uint8_t)g); nvs_commit(h); nvs_close(h);
}

static void rec_sink(const uint8_t *a, size_t n, const ima_state_t *st,
                     uint16_t fno, void *ctx) {
    (void)st; (void)fno; (void)ctx;
    if (s_rec_len + n > s_rec_cap) { s_rec_full = true; return; }
    memcpy(s_rec + s_rec_len, a, n);
    s_rec_len += n;
}
static void null_sink(const uint8_t *a, size_t n, const ima_state_t *st,
                      uint16_t fno, void *ctx) {
    (void)a; (void)n; (void)st; (void)fno; (void)ctx;
}

static void print_stats(void) {
    ear_audio_stats_t s; ear_audio_get_stats(&s);
    uprintf("frames=%lu dc_raw=%d rms=%d agc=x%d (max x%d) peak=%d clips=%lu\r\n",
            (unsigned long)s.frames, s.dc_raw, s.rms, ear_audio_gain_now(),
            ear_audio_gain(), s.peak, (unsigned long)s.clips);
}

/* ( -- ) 1 s level check */
static void w_mic_stat(void) {
    if (ear_audio_running()) { uprint("\r\nbusy (stream open)\r\n"); return; }
    ear_audio_start(REC_FRAME, null_sink, NULL);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ear_audio_stop();
    uprint("\r\n"); print_stats();
}

/* ( secs -- ) record ADPCM into RAM */
static void w_mic_rec(void) {
    int secs = (int)forth_pop();
    if (secs < 1) secs = 1;
    if (secs > REC_MAX_SECS) secs = REC_MAX_SECS;
    if (ear_audio_running()) { uprint("\r\nbusy (stream open)\r\n"); return; }
    size_t cap = (size_t)secs * (EAR_AUDIO_RATE / 2);
    if (cap > s_rec_cap) {
        free(s_rec); s_rec = malloc(cap); s_rec_cap = s_rec ? cap : 0;
        if (!s_rec) { uprint("\r\nno memory\r\n"); return; }
    }
    s_rec_len = 0; s_rec_full = false;
    uprintf("\r\nrecording %d s — talk now...\r\n", secs);
    board_led(true);
    ear_audio_start(REC_FRAME, rec_sink, NULL);
    int64_t until = esp_timer_get_time() + (int64_t)secs * 1000000;
    while (!s_rec_full && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(20));
    ear_audio_stop();
    board_led(false);
    uprintf("recorded %u ADPCM bytes (%.2f s)\r\n", (unsigned)s_rec_len,
            s_rec_len / (EAR_AUDIO_RATE / 2.0));
    print_stats();
}

/* ( -- ) dump last recording as base64 between markers (tools/ear_dump.py) */
static void w_mic_dump(void) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!s_rec_len) { uprint("\r\nnothing recorded (n mic-rec)\r\n"); return; }
    uprintf("\r\n-----BEGIN EAR ADPCM rate=%d pred=0 idx=0 bytes=%u-----\r\n",
            EAR_AUDIO_RATE, (unsigned)s_rec_len);
    char line[80]; int lp = 0;
    for (size_t i = 0; i < s_rec_len; i += 3) {
        uint32_t v = (uint32_t)s_rec[i] << 16;
        size_t k = s_rec_len - i;
        if (k > 1) v |= (uint32_t)s_rec[i + 1] << 8;
        if (k > 2) v |= s_rec[i + 2];
        line[lp++] = b64[(v >> 18) & 63];
        line[lp++] = b64[(v >> 12) & 63];
        line[lp++] = k > 1 ? b64[(v >> 6) & 63] : '=';
        line[lp++] = k > 2 ? b64[v & 63] : '=';
        if (lp >= 76) { line[lp] = 0; uprint(line); uprint("\r\n"); lp = 0; }
    }
    if (lp) { line[lp] = 0; uprint(line); uprint("\r\n"); }
    uprint("-----END EAR ADPCM-----\r\n");
}

/* ( n -- ) */
static void w_ear_gain(void) {
    int g = (int)forth_pop();
    ear_audio_set_gain(g);
    gain_save(ear_audio_gain());
    uprintf("\r\ngain = %d\r\n", ear_audio_gain());
}
static void w_ear_gain_q(void) { uprintf("\r\ngain: %d\r\n", ear_audio_gain()); }

/* ( -- ) codec round-trip SNR on a synthetic 440 Hz + 1.3 kHz signal */
static void w_ear_selftest(void) {
    enum { N = 1024 };
    int16_t *in = malloc(N * 2), *out = malloc(N * 2);
    uint8_t *enc = malloc(N / 2);
    if (!in || !out || !enc) { uprint("\r\nno memory\r\n"); goto done; }
    for (int i = 0; i < N; i++)
        in[i] = (int16_t)(8000 * sinf(2 * M_PI * 440 * i / 16000.0f) +
                          3000 * sinf(2 * M_PI * 1300 * i / 16000.0f));
    ima_state_t e = {0, 0}, d = {0, 0};
    ima_encode(&e, in, N, enc);
    ima_decode(&d, enc, N / 2, out);
    double sig = 0, err = 0;
    for (int i = 64; i < N; i++) {            /* skip adaptation warm-up */
        sig += (double)in[i] * in[i];
        err += (double)(in[i] - out[i]) * (in[i] - out[i]);
    }
    double snr = 10.0 * log10(sig / (err > 0 ? err : 1));
    bool sync = e.predictor == d.predictor && e.step_index == d.step_index;
    uprintf("\r\nADPCM round-trip SNR %.1f dB, enc/dec state %s -> %s\r\n",
            snr, sync ? "in sync" : "DIVERGED", (snr > 20 && sync) ? "PASS" : "FAIL");
done:
    free(in); free(out); free(enc);
}

/* ---------- Forth FFI ---------- */
static void w_ear_mem(void) { uprint("\r\n"); mem_report("now"); }

static void w_ear_stat(void) {
    board_batt_t b; board_batt_read(&b);
    uprintf("\r\nfw:      %s\r\n", EAR_FW_VERSION);
    uprintf("node:    %s\r\n",     node_id);
    uprintf("ble:     %s  %s\r\n", ear_ble_name(), ear_ble_addr());
    uprintf("link:    conns=%d adv=%d encrypted=%d bonds=%d\r\n",
            ear_ble_conn_count(), ear_ble_advertising(), ear_ble_encrypted(),
            ear_ble_bond_count());
    uprintf("batt:    %d mV  %d%%  %s  %d mA\r\n",
            b.mv, b.pct, b.charging ? "charging" : "discharging", b.dischg_ma);
    atvv_status_t at; ear_atvv_status(&at);
    static const char *st[] = { "idle", "ready", "LIVE" };
    uprintf("atvv:    %s%s reason=0x%02x frame=%u mtu=%u interval=%u.%02u ms sent=%lu dropped=%lu queued=%u ms\r\n",
            st[at.state], at.waiting_host ? " (waiting for host)" : "", at.reason, at.frame_bytes,
            at.mtu, at.conn_itvl_x100 / 100, at.conn_itvl_x100 % 100,
            (unsigned long)at.frames_sent, (unsigned long)at.frames_dropped, at.buffered_ms);
    uprintf("audio:   agc x%d (max x%d)\r\n", ear_audio_gain_now(), ear_audio_gain());
    mem_report("now");
}

/* ( secs -- ) simulate holding the thumb button (bench/HTT test) */
static void w_talk(void) {
    int secs = (int)forth_pop();
    if (secs < 1) secs = 1;
    if (secs > 30) secs = 30;
    uprintf("\r\ntalk %d s (simulated button A hold)\r\n", secs);
    ear_atvv_button(true);
    vTaskDelay(pdMS_TO_TICKS(secs * 1000));
    ear_atvv_button(false);
}

static void w_power_off(void) {
    uprint("\r\npowering off (power key turns it back on)\r\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    board_power_off();
}
/* ( reg -- ) */
static void w_axp_fetch(void) {
    int r = (int)forth_pop();
    uprintf("\r\nAXP192[0x%02x] = 0x%02x\r\n", r, board_axp_read(r));
}

static void w_ble_pair(void) {
    ear_ble_pairing(true, PAIR_WINDOW_S);
    uprint("\r\npairing window open 120 s\r\n");
}

static void w_ble_unpair(void) {
    ear_ble_unpair_all();
    uprint("\r\nall bonds cleared\r\n");
}

static void register_forth_words(void) {
    forth_register_word("ear-mem",    w_ear_mem);
    forth_register_word("ear-stat",   w_ear_stat);
    forth_register_word("ble-pair",   w_ble_pair);
    forth_register_word("power-off",  w_power_off);
    forth_register_word("axp@",       w_axp_fetch);
    forth_register_word("ble-unpair", w_ble_unpair);
    forth_register_word("talk",       w_talk);
    forth_register_word("mic-stat",   w_mic_stat);
    forth_register_word("mic-rec",    w_mic_rec);
    forth_register_word("mic-dump",   w_mic_dump);
    forth_register_word("ear-gain",   w_ear_gain);
    forth_register_word("ear-gain?",  w_ear_gain_q);
    forth_register_word("ear-selftest", w_ear_selftest);
}

/* ---------- app_main ---------- */
void app_main(void) {
    console_init();
    vTaskDelay(pdMS_TO_TICKS(300));

    uprint("\r\n\r\n====================================\r\n");
    uprint(  "  MagNET Hive Ear (Role 13) — BLE accessory\r\n");
    uprintf( "  fw %s  ESPIDFORTH %s\r\n", EAR_FW_VERSION, ESPIDFORTH_VERSION_STRING);
    uprintf( "  MagNET gen %s\r\n", MAGNET_GEN_STR);
    uprint(  "  M5StickC Plus (ESP32-PICO-D4, no PSRAM)\r\n");
    uprint(  "====================================\r\n");
    mem_report("boot");

    /* M5GFX first: it owns I2C port 1 (AXP192) and powers the panel. */
    board_init();
    derive_ids();
    board_screen_t splash = { "EAR", BOARD_TONE_NORMAL, false,
                              "booting...", "", "", "", "", false };
    board_draw(&splash);
    mem_report("board");

    esp_err_t nrc = nvs_flash_init();     /* NimBLE bond store */
    if (nrc == ESP_ERR_NVS_NO_FREE_PAGES || nrc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Forth before BLE, or the dictionary heap starves. */
    forth_init(FORTH_HEAP_SIZE);
    register_forth_words();
    mem_report("forth");

    /* BLE-only controller: hand Classic-BT controller memory back first. */
    esp_err_t mr = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    ESP_LOGI(TAG, "classic BT mem release: %s", esp_err_to_name(mr));
    mem_report("bt-classic-rel");

    if (ear_atvv_init() != 0) uprint("[ATVV] init FAILED\r\n");

    ear_ble_config_t bcfg = { .model = EAR_MODEL, .fw_rev = EAR_FW_VERSION, .serial = node_id };
    if (ear_ble_init(&bcfg, on_ble) != 0)
        uprint("[BLE] init FAILED\r\n");
    vTaskDelay(pdMS_TO_TICKS(300));       /* let the host sync + start advertising */
    uprintf("BLE: %s (%s)  node %s\r\n", ear_ble_name(), ear_ble_addr(), node_id);
    mem_report("ble-init");

    /* Mic last: its CLK is GPIO0 (boot strap) — only touch it after boot. */
    if (ear_audio_init() != 0) uprint("[MIC] init FAILED\r\n");
    gain_load();
    mem_report("audio-init");

    xTaskCreate(ui_task, "ui", 4096, NULL, 2, NULL);

    uprint("\r\nForth commands:\r\n");
    uprint("  ear-mem / ear-stat   -- internal heap / battery / BLE link\r\n");
    uprint("  ble-pair / ble-unpair -- open 120 s pairing window / forget all bonds\r\n");
    uprint("  mic-stat             -- 1 s level / DC / clip check\r\n");
    uprint("  N mic-rec / mic-dump -- record N s ADPCM, dump base64 (tools/ear_dump.py)\r\n");
    uprint("  N ear-gain / ear-gain? / ear-selftest\r\n");
    uprint("  N talk               -- simulate holding button A for N s\r\n");
    uprint("  power-off / N axp@   -- AXP192 shutdown / read a register\r\n");
    uprint("  side button B: press = pairing window, hold 5 s = forget bonds + reboot\r\n");
    uprint("  power key (bottom edge): hold 1.5 s = power off\r\n\r\n");

    forth_repl(console_getchar, console_putchar);
    forth_deinit();
}
