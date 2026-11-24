/*
 * M5StickCPlus_Hive_Sentinel_IR — people-presence sentinel on a PIR sensor.
 *
 * A RadioShack 276-0347 PIR (3-pin: VCC / OUT / GND) on the StickC Plus top
 * header watches a room. Motion turns into presence (presence.c): one
 * "enter" per arrival, a "present" re-ping every 5 min while motion keeps
 * coming, and a "clear" once the room has been still for the hold time.
 * Events go out over MQTT to the usual MagNET broker so a subscriber on any
 * network can follow along; counts and occupied time per hour / day / week
 * / month (stats.c) show on screen and reset at each period boundary or
 * from the menu.
 *
 * Forked from M5StickCPlus_Hive_Ear (board layer, UI legends, power key);
 * WiFi / MQTT / console plumbing follows M5StickCPlus-Blinky_Voice_XR.
 * WiFi + MQTT only — Bluetooth is compiled out.
 *
 * MQTT (base = iotj/magnet/sentinel/<mac4>, see README):
 *   <base>/event  QoS 1        enter | present | clear | rollover | reset
 *   <base>/state  QoS 1 retain current state + stats; LWT {"state":"offline"}
 *   <base>/activity  QoS 0 retain  motion now + last 5 min (30 x 10 s)
 *   <base>/history   QoS 1 retain  occupied s per 15 min, last 24 h (96 bins)
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "nvs.h"

#include "craw_serial.h"
#include "craw_wifi.h"
#include "craw_nvs.h"
#include "craw_mqtt.h"
#include "forth_core.h"
#include "forth_version.h"
#include "board_stickc.h"
#include "presence.h"
#include "stats.h"
#include "activity.h"
#include "net_portal.h"
#include "../../include/magnet_gen.h"

static const char *TAG = "sentinel";

#define SEN_FW_VERSION    "0.3.1"
#define FORTH_HEAP_SIZE   (32 * 1024)
#define HOSTNAME_PREFIX   "MagNET-Sentinel"

#define DEF_HOLD_S        300      /* no motion this long = room clear */
#define DEF_REPEAT_S      300      /* "still here" re-ping while present */
#define WARMUP_S          30       /* PIR settle after power-up */
#define DEF_QUIET_H       12       /* no motion this long: "check sensor" */
#define DEF_TZ            "PST8PDT,M3.2.0,M11.1.0"
#define DEF_BRIGHT        60
/* Guest networks often block 1883 even after the portal; HiveMQ also takes
 * MQTT over TLS WebSockets on 8884. Tried after MQTT_SWAP_S of "internet
 * open, MQTT down", then the two alternate. */
#define DEF_MQTT_ALT      "wss://broker.hivemq.com:8884/mqtt"
#define MQTT_SWAP_S       60

#define PIR_POLL_MS       50
#define PIR_STABLE_N      2        /* samples: 100 ms glitch filter */
#define STATS_SAVE_MS     (10 * 60 * 1000)
#define DIM_AFTER_MS      60000
#define DIM_PCT           8
#define MENU_TIMEOUT_MS   30000
#define TOAST_MS          4000

#define TX_DEPTH          10       /* events held while MQTT is down */
#define JSON_MAX          640

#define SEN_NS            "sentinel"

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ---------- identity + config ---------- */
static char mac4[8], hostname[32];
static char mqtt_broker[128];
static char mqtt_alt[128] = DEF_MQTT_ALT;
static const char *mqtt_active;          /* mqtt_broker or mqtt_alt */
static char topic_base[96], topic_event[112], topic_state[112], topic_activity[112], topic_history[112];
static char wifi_ssid[33], wifi_pass[65];
static char tz[64] = DEF_TZ;
static int  bright_pct = DEF_BRIGHT;

static void derive_ids(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac4, sizeof(mac4), "%02x%02x", mac[4], mac[5]);
    snprintf(hostname, sizeof(hostname), "%s-%s", HOSTNAME_PREFIX, mac4);
}

static void set_topics(void) {
    snprintf(topic_event, sizeof(topic_event), "%s/event", topic_base);
    snprintf(topic_state, sizeof(topic_state), "%s/state", topic_base);
    snprintf(topic_activity, sizeof(topic_activity), "%s/activity", topic_base);
    snprintf(topic_history, sizeof(topic_history), "%s/history", topic_base);
}

static void nvs_get_s(const char *key, char *buf, size_t n) {
    nvs_handle_t h; size_t len = n;
    if (nvs_open(SEN_NS, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_str(h, key, buf, &len);
    nvs_close(h);
}
static void nvs_set_s(const char *key, const char *v) {
    nvs_handle_t h;
    if (nvs_open(SEN_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, v); nvs_commit(h); nvs_close(h);
}
static uint32_t nvs_get_u(const char *key, uint32_t def) {
    nvs_handle_t h; uint32_t v = def;
    if (nvs_open(SEN_NS, NVS_READONLY, &h) != ESP_OK) return def;
    if (nvs_get_u32(h, key, &v) != ESP_OK) v = def;
    nvs_close(h);
    return v;
}
static void nvs_set_u(const char *key, uint32_t v) {
    nvs_handle_t h;
    if (nvs_open(SEN_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, v); nvs_commit(h); nvs_close(h);
}

/* ---------- shared state (s_lock) ---------- */
static SemaphoreHandle_t s_lock;
static presence_t s_pres;
static stats_t    s_stats;
static int64_t    s_credit_ms;       /* occupied time credited up to here */
static int64_t    s_changed_ms;      /* last presence state change */
static bool       s_occ_dirty;
static volatile int64_t s_sim_until; /* pir-sim: fake motion until */
static hist_t     s_hist;            /* occupied s per 15 min, 24 h */
static recent_t   s_recent;          /* motion per 10 s, 5 min */
static volatile bool s_wake;         /* state changed: light the screen */
/* A PIR that has fallen off its header reads exactly like an empty room
 * (the pull-down holds G26 low). The only tell is silence, so flag a
 * sensor that has seen nothing for quiet_h hours. */
static uint32_t   s_quiet_h = DEF_QUIET_H;
static bool       s_quiet;

/* Seconds since the PIR last saw motion, or since warm-up ended if it has
 * not seen any yet. Caller holds s_lock. */
static int64_t quiet_s(int64_t now) {
    if (s_pres.state == PRES_WARMUP) return 0;
    int64_t ref = s_pres.t_motion_ms ? s_pres.t_motion_ms
                                     : s_pres.t_start_ms + (int64_t)s_pres.warmup_s * 1000;
    return (now - ref) / 1000;
}

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static bool clock_ok(time_t t) { return t > 1700000000; }   /* SNTP has landed */

static bool local_now(struct tm *lt) {
    time_t t = time(NULL);
    if (!clock_ok(t)) return false;
    localtime_r(&t, lt);
    return true;
}

static void stats_load(void) {
    nvs_handle_t h; size_t len = sizeof(s_stats);
    stats_init(&s_stats);
    if (nvs_open(SEN_NS, NVS_READONLY, &h) != ESP_OK) return;
    stats_t tmp;
    if (nvs_get_blob(h, "stats", &tmp, &len) == ESP_OK && len == sizeof(tmp) &&
        tmp.magic == STATS_MAGIC)
        s_stats = tmp;
    nvs_close(h);
}
static void hist_load(void) {
    nvs_handle_t h; size_t len = sizeof(s_hist);
    hist_init(&s_hist);
    if (nvs_open(SEN_NS, NVS_READONLY, &h) != ESP_OK) return;
    hist_t tmp;
    if (nvs_get_blob(h, "hist", &tmp, &len) == ESP_OK && len == sizeof(tmp) &&
        tmp.magic == HIST_MAGIC)
        s_hist = tmp;
    nvs_close(h);
}
static void stats_save(void) {           /* caller holds s_lock; saves history too */
    nvs_handle_t h;
    if (nvs_open(SEN_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "stats", &s_stats, sizeof(s_stats));
    nvs_set_blob(h, "hist", &s_hist, sizeof(s_hist));
    nvs_commit(h); nvs_close(h);
    s_occ_dirty = false;
}

/* ---------- MQTT outbox ---------- */
typedef struct { char json[JSON_MAX]; } msg_t;
static QueueHandle_t s_txq;
static volatile bool s_state_dirty = true;
static volatile bool s_act_dirty = true;     /* <base>/activity due */
static volatile bool s_hist_dirty = true;    /* <base>/history changed */
static volatile bool s_hist_now = true;      /* ... and send it without waiting */
static volatile uint32_t s_tx_dropped;

static void app(char *b, size_t cap, int *n, const char *fmt, ...) {
    if (*n >= (int)cap) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(b + *n, cap - *n, fmt, ap);
    va_end(ap);
    *n = k < 0 ? (int)cap : *n + k;
}

/* {"id":..,"ev":..,"state":..,"ts":..,"local":..,"up_s":..,"since_s":..<extra>,"stats":{..}}
 * Caller holds s_lock. */
static int build_json(char *b, size_t cap, const char *ev, const char *extra) {
    int n = 0;
    time_t t = time(NULL);
    char ts[16] = "null", local[32] = "";
    if (clock_ok(t)) {
        struct tm lt; localtime_r(&t, &lt);
        snprintf(ts, sizeof(ts), "%lld", (long long)t);
        strftime(local, sizeof(local), "%Y-%m-%dT%H:%M:%S%z", &lt);
    }
    int64_t now = now_ms();
    app(b, cap, &n, "{\"id\":\"%s\",\"ev\":\"%s\",\"state\":\"%s\",\"ts\":%s,\"local\":\"%s\","
                    "\"up_s\":%lld,\"since_s\":%lld%s,\"stats\":{",
        mac4, ev, presence_state_name(s_pres.state), ts, local,
        (long long)(now / 1000), (long long)((now - s_changed_ms) / 1000), extra ? extra : "");
    for (int p = 0; p < ST_N; p++)
        app(b, cap, &n, "%s\"%s\":{\"entries\":%lu,\"occ_s\":%lu}", p ? "," : "",
            stats_period_name(p), (unsigned long)s_stats.entries[p],
            (unsigned long)s_stats.occ_s[p]);
    app(b, cap, &n, "}}");
    return n;
}

/* Queue an event for <base>/event; drops the oldest if MQTT has been down
 * long enough to fill the outbox. Caller holds s_lock. */
static void emit(const char *ev, const char *extra) {
    static msg_t m, old;
    if (build_json(m.json, sizeof(m.json), ev, extra) >= JSON_MAX)
        ESP_LOGW(TAG, "event json truncated");
    if (xQueueSend(s_txq, &m, 0) != pdTRUE) {
        xQueueReceive(s_txq, &old, 0);
        s_tx_dropped++;
        xQueueSend(s_txq, &m, 0);
    }
    s_state_dirty = true;
}

static void mqtt_setup(const char *uri);

/* Internet open but MQTT still down: switch between the main broker
 * address and the alternate (TLS WebSocket) one. */
static void mqtt_maybe_swap(void) {
    static int64_t last_swap;
    if (!mqtt_alt[0] || net_portal_mqtt_stuck_s() < MQTT_SWAP_S) return;
    if (now_ms() - last_swap < MQTT_SWAP_S * 1000LL) return;
    last_swap = now_ms();
    const char *next = mqtt_active == mqtt_alt ? mqtt_broker : mqtt_alt;
    craw_serial_printf("\r\n[MQTT] no connection to %s for %ds with the internet open; trying %s\r\n",
                       mqtt_active, MQTT_SWAP_S, next);
    craw_mqtt_stop();
    mqtt_setup(next);
    craw_mqtt_start();
}

/* {"id","active","state","ts","last_motion_s","clear_in_s","bin_s":10,"bins_pct":[30]}
 * Caller holds s_lock. */
static int build_activity(char *b, size_t cap) {
    int n = 0;
    int64_t now = now_ms();
    recent_advance(&s_recent, now);
    time_t t = time(NULL);
    char ts[24] = "null", lm[24] = "null", ci[24] = "null";
    if (clock_ok(t)) snprintf(ts, sizeof(ts), "%lld", (long long)t);
    if (s_pres.t_motion_ms) snprintf(lm, sizeof(lm), "%lld", (long long)((now - s_pres.t_motion_ms) / 1000));
    if (s_pres.state == PRES_PRESENT) {
        int64_t left = (int64_t)s_pres.hold_s * 1000 - (now - s_pres.t_motion_ms);
        snprintf(ci, sizeof(ci), "%lld", (long long)(left > 0 ? (left + 999) / 1000 : 0));
    }
    app(b, cap, &n, "{\"id\":\"%s\",\"active\":%s,\"state\":\"%s\",\"ts\":%s,"
                    "\"last_motion_s\":%s,\"clear_in_s\":%s,\"hold_s\":%lu,"
                    "\"quiet_s\":%lld,\"quiet_after_s\":%lu,\"check_sensor\":%s,"
                    "\"bin_s\":%d,\"bins_pct\":[",
        mac4, s_pres.motion ? "true" : "false", presence_state_name(s_pres.state), ts, lm, ci,
        (unsigned long)s_pres.hold_s, (long long)quiet_s(now), (unsigned long)s_quiet_h * 3600,
        s_quiet ? "true" : "false", RECENT_BIN_MS / 1000);
    for (int i = 0; i < RECENT_BINS; i++)
        app(b, cap, &n, "%s%u", i ? "," : "", (unsigned)((s_recent.active_ms[i] + 50) / 100));
    // The counters too, so subscribers' figures stay fresh between events.
    app(b, cap, &n, "],\"stats\":{");
    for (int p = 0; p < ST_N; p++)
        app(b, cap, &n, "%s\"%s\":{\"entries\":%lu,\"occ_s\":%lu}", p ? "," : "",
            stats_period_name(p), (unsigned long)s_stats.entries[p],
            (unsigned long)s_stats.occ_s[p]);
    app(b, cap, &n, "}}");
    return n;
}

/* {"id","ts","bin_s":900,"end":<unix start of newest bin>|null,"bins_s":[96]}, oldest first.
 * Caller holds s_lock. */
static int build_history(char *b, size_t cap) {
    int n = 0;
    time_t t = time(NULL);
    char ts[24] = "null", end[24] = "null";
    if (clock_ok(t)) snprintf(ts, sizeof(ts), "%lld", (long long)t);
    if (s_hist.key) snprintf(end, sizeof(end), "%lld", (long long)s_hist.key * HIST_BIN_S);
    app(b, cap, &n, "{\"id\":\"%s\",\"ts\":%s,\"bin_s\":%d,\"end\":%s,\"bins_s\":[",
        mac4, ts, HIST_BIN_S, end);
    for (int i = 0; i < HIST_BINS; i++)
        app(b, cap, &n, "%s%u", i ? "," : "", (unsigned)s_hist.occ_s[i]);
    app(b, cap, &n, "]}");
    return n;
}

static void tx_task(void *arg) {
    (void)arg;
    static msg_t m;
    static char big[900];
    int64_t last_hist = 0;
    for (;;) {
        if (!craw_mqtt_is_connected()) {
            mqtt_maybe_swap();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s_act_dirty) {
            s_act_dirty = false;
            LOCK();
            build_activity(big, sizeof(big));
            UNLOCK();
            if (craw_mqtt_publish(topic_activity, big, 0, true) < 0) s_act_dirty = true;
        }
        // History changes every second while someone is present; once a
        // minute is plenty for a 15-min chart. Bin rollovers go out at once.
        if (s_hist_dirty && (s_hist_now || now_ms() - last_hist >= 60000)) {
            s_hist_dirty = s_hist_now = false;
            last_hist = now_ms();
            LOCK();
            build_history(big, sizeof(big));
            UNLOCK();
            if (craw_mqtt_publish(topic_history, big, 1, true) < 0) s_hist_dirty = s_hist_now = true;
        }
        if (s_state_dirty) {
            s_state_dirty = false;
            LOCK();
            build_json(m.json, sizeof(m.json), "state",
                       s_quiet ? ",\"online\":true,\"check_sensor\":true"
                               : ",\"online\":true,\"check_sensor\":false");
            UNLOCK();
            if (craw_mqtt_publish(topic_state, m.json, 1, true) < 0) s_state_dirty = true;
        }
        if (xQueuePeek(s_txq, &m, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (craw_mqtt_publish(topic_event, m.json, 1, false) >= 0)
                xQueueReceive(s_txq, &m, 0);
            else
                vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/* ---------- presence + stats ---------- */
static char    s_toast[24];
static board_batt_t s_batt = { .pct = -1 };   /* ui_task owns the I2C reads */
static int64_t s_toast_until;

static void toast(const char *t) {
    snprintf(s_toast, sizeof(s_toast), "%s", t);
    s_toast_until = now_ms() + TOAST_MS;
}

static void clock_str(char *b, size_t n) {
    struct tm lt;
    if (local_now(&lt)) strftime(b, n, "%H:%M:%S", &lt);
    else snprintf(b, n, "up %llds", (long long)(now_ms() / 1000));
}

/* Credit occupied seconds up to the last motion — not up to now, so the
 * hold-window tail after someone leaves isn't counted. Motion only moves
 * while present (ENTER resets the credit mark), so no state check is
 * needed, and CLEAR can credit the visit's tail. Caller holds s_lock. */
static void credit_occ(void) {
    if (s_pres.t_motion_ms <= s_credit_ms) return;
    uint32_t secs = (uint32_t)((s_pres.t_motion_ms - s_credit_ms) / 1000);
    if (!secs) return;
    stats_add_occ(&s_stats, secs);
    hist_add(&s_hist, secs);
    s_hist_dirty = true;
    s_credit_ms += (int64_t)secs * 1000;
    s_occ_dirty = true;
}

/* Close any calendar period that has ended. Caller holds s_lock. */
static void roll_periods(void) {
    struct tm lt;
    if (!local_now(&lt)) return;
    stats_t closed;
    unsigned mask = stats_roll(&s_stats, &lt, &closed);
    for (int p = 0; p < ST_N; p++) {
        if (!(mask & (1u << p))) continue;
        char label[24], extra[160];
        stats_label(p, closed.key[p], label, sizeof(label));
        snprintf(extra, sizeof(extra),
                 ",\"period\":\"%s\",\"label\":\"%s\",\"entries\":%lu,\"occ_s\":%lu",
                 stats_period_name(p), label, (unsigned long)closed.entries[p],
                 (unsigned long)closed.occ_s[p]);
        emit("rollover", extra);
        craw_serial_printf("\r\n[STATS] %s %s closed: %lu entries, %lu s occupied\r\n",
                           stats_period_name(p), label, (unsigned long)closed.entries[p],
                           (unsigned long)closed.occ_s[p]);
    }
    if (mask) stats_save();
}

static void on_presence(pres_event_t e, int64_t now) {    /* caller holds s_lock */
    char extra[48], clk[24];
    clock_str(clk, sizeof(clk));
    switch (e) {
    case PRES_EV_NONE:
        return;
    case PRES_EV_READY:
        s_changed_ms = now;
        s_wake = true;
        s_state_dirty = true;
        craw_serial_printf("\r\n[PIR] warmed up — watching\r\n");
        return;
    case PRES_EV_ENTER:
        roll_periods();
        stats_add_entry(&s_stats);
        s_credit_ms = now;
        s_changed_ms = now;
        s_wake = true;
        emit("enter", "");
        stats_save();
        toast("SOMEONE ENTERED");
        break;
    case PRES_EV_STILL:
        snprintf(extra, sizeof(extra), ",\"visit_s\":%lld",
                 (long long)((now - s_pres.t_enter_ms) / 1000));
        emit("present", extra);
        break;
    case PRES_EV_CLEAR:
        credit_occ();
        s_changed_ms = now;
        s_wake = true;
        snprintf(extra, sizeof(extra), ",\"visit_s\":%lld",
                 (long long)((s_pres.t_motion_ms - s_pres.t_enter_ms) / 1000));
        emit("clear", extra);
        stats_save();
        toast("ROOM CLEAR");
        break;
    }
    craw_serial_printf("\r\n[PIR] %s %s  hour %lu day %lu week %lu month %lu\r\n",
                       presence_event_name(e), clk,
                       (unsigned long)s_stats.entries[ST_HOUR], (unsigned long)s_stats.entries[ST_DAY],
                       (unsigned long)s_stats.entries[ST_WEEK], (unsigned long)s_stats.entries[ST_MONTH]);
}

static void sensor_task(void *arg) {
    (void)arg;
    bool level = false, cand = false;
    int stable = 0;
    int64_t last_tick = 0, last_save = now_ms(), prev = now_ms(), last_act = 0;
    bool prev_level = false, act_nonzero = false;
    for (;;) {
        int64_t now = now_ms();
        bool raw = board_pir_raw() || now < s_sim_until;
        if (raw == cand) { if (stable < PIR_STABLE_N) stable++; }
        else { cand = raw; stable = 1; }
        if (stable >= PIR_STABLE_N) level = cand;
        board_led(level);

        LOCK();
        recent_add(&s_recent, now, level ? (uint32_t)(now - prev) : 0);
        prev = now;
        on_presence(presence_step(&s_pres, now, level), now);
        // Motion starting or stopping goes out at once; while there is
        // anything in the 5-min window, refresh every 10 s so the chart
        // scrolls, then once more when it has emptied.
        bool nonzero = level || recent_total_ms(&s_recent) > 0;
        if (level != prev_level || ((nonzero || act_nonzero) && now - last_act >= RECENT_BIN_MS)) {
            s_act_dirty = true;
            last_act = now;
            act_nonzero = nonzero;
        }
        prev_level = level;
        if (now - last_tick >= 1000) {
            last_tick = now;
            bool quiet = quiet_s(now) >= (int64_t)s_quiet_h * 3600;
            if (quiet != s_quiet) {
                s_quiet = quiet;
                s_state_dirty = s_act_dirty = true;
                if (quiet) s_wake = true;
                if (quiet)
                    craw_serial_printf("\r\n[PIR] no motion for %luh — check the sensor wiring\r\n",
                                       (unsigned long)s_quiet_h);
                else
                    craw_serial_print("\r\n[PIR] motion again — sensor OK\r\n");
            }
            time_t t = time(NULL);
            if (clock_ok(t) && hist_advance(&s_hist, (int32_t)(t / HIST_BIN_S))) {
                s_hist_dirty = s_hist_now = true;
                stats_save();
            }
            // The state sent on MQTT connect usually predates SNTP; resend it
            // once the clock is real so subscribers get a timestamp.
            static bool had_clock;
            if (!had_clock && clock_ok(time(NULL))) { had_clock = true; s_state_dirty = true; }
            credit_occ();
            roll_periods();
            if (s_occ_dirty && now - last_save >= STATS_SAVE_MS) { stats_save(); last_save = now; }
        }
        UNLOCK();
        vTaskDelay(pdMS_TO_TICKS(PIR_POLL_MS));
    }
}

/* Zero periods by request (menu or console) and tell subscribers. */
static void stats_reset_req(unsigned mask) {
    LOCK();
    credit_occ();
    struct tm lt;
    stats_reset(&s_stats, mask, local_now(&lt) ? &lt : NULL);
    char extra[96]; int n = 0;
    app(extra, sizeof(extra), &n, ",\"periods\":[");
    for (int p = 0, first = 1; p < ST_N; p++)
        if (mask & (1u << p)) { app(extra, sizeof(extra), &n, "%s\"%s\"", first ? "" : ",", stats_period_name(p)); first = 0; }
    app(extra, sizeof(extra), &n, "]");
    emit("reset", extra);
    stats_save();
    UNLOCK();
    craw_serial_printf("\r\n[STATS] reset %s\r\n", extra + 1);
}

/* ---------- WiFi / MQTT / clock ---------- */
static bool s_sntp_started;
static volatile bool s_wifi_failed;   /* a whole retry burst failed; cleared on connect */

static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    (void)ctx;
    switch (event) {
    case CRAW_WIFI_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected");
        if (!s_sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&cfg);
            s_sntp_started = true;
        }
        s_wifi_failed = false;
        craw_mqtt_start();
        net_portal_wifi(true);
        break;
    case CRAW_WIFI_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WiFi disconnected");
        net_portal_wifi(false);
        break;
    case CRAW_WIFI_EVENT_CONNECT_FAILED:
        ESP_LOGW(TAG, "WiFi connect failed");
        s_wifi_failed = true;
        break;
    }
}

/* From the setup page (net task). */
static void wifi_saved(const char *ssid, const char *pass) {
    snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid);
    snprintf(wifi_pass, sizeof(wifi_pass), "%s", pass);
    craw_nvs_save_wifi_creds(wifi_ssid, wifi_pass);
    craw_serial_printf("\r\n[NET] WiFi set from the setup page: '%s'\r\n", wifi_ssid);
}

static void on_mqtt_conn(bool connected, void *ctx) {
    (void)ctx;
    // Overwrite the broker's retained "offline" last will straight away.
    if (connected) s_state_dirty = s_act_dirty = s_hist_dirty = s_hist_now = true;
    craw_serial_printf("\r\n[MQTT] %s\r\n", connected ? "connected" : "disconnected");
}

static void mqtt_setup(const char *uri) {
    static char lwt[96];
    snprintf(lwt, sizeof(lwt), "{\"id\":\"%s\",\"ev\":\"state\",\"state\":\"offline\",\"online\":false}", mac4);
    mqtt_active = uri;
    craw_mqtt_init(uri, "", NULL, on_mqtt_conn, NULL);
    craw_mqtt_set_lwt(topic_state, lwt, 1, true, 30);
}

static void tz_apply(void) {
    setenv("TZ", tz, 1);
    tzset();
}

static const char *net_line(void) {
    if (net_portal_state() == NET_SETUP) return "WiFi setup open";
    if (!wifi_ssid[0])                return "no WiFi: menu > WIFI SETUP";
    if (!craw_wifi_is_connected())
        return s_wifi_failed ? "WiFi failing: menu>WIFI SETUP" : "WiFi connecting...";
    switch (net_portal_state()) {
    case NET_CHECKING:    return "checking internet...";
    case NET_ACCEPTING:   return "accepting sign-in page...";
    case NET_PORTAL:      return "WiFi needs a sign-in";
    case NET_RELAY:       return "phone login open";
    case NET_NO_INTERNET: return "WiFi up, no internet";
    default: break;
    }
    if (!craw_mqtt_is_connected())    return "MQTT connecting...";
    return uxQueueMessagesWaiting(s_txq) ? "MQTT sending..." : "MQTT online";
}

/* ---------- UI ---------- */
typedef enum { PG_HOME, PG_STATS, PG_MENU, PG_CONFIRM, PG_INFO } page_t;
static const char *const MENU[] = {
    "RESET HOUR", "RESET DAY", "RESET WEEK", "RESET MONTH", "RESET ALL", "WIFI LOGIN",
    "WIFI SETUP", "DEVICE INFO", "EXIT",
};
enum { MI_N = sizeof(MENU) / sizeof(MENU[0]), MI_ALL = 4, MI_LOGIN = 5, MI_SETUP = 6, MI_INFO = 7,
       MI_EXIT = 8 };

static unsigned menu_mask(int item) { return item == MI_ALL ? ST_ALL : 1u << item; }

/* Elapsed time that can't be mistaken for a clock time, and only changes
 * once a minute (every change redraws the panel): "just now", "13m", "2h05m". */
static void fmt_dur(char *b, size_t n, int secs) {
    if (secs < 60)        snprintf(b, n, "just now");
    else if (secs < 3600) snprintf(b, n, "%dm", secs / 60);
    else                  snprintf(b, n, "%dh%02dm", secs / 3600, secs / 60 % 60);
}

static void fmt_hm(char *b, size_t n, uint32_t secs) {
    snprintf(b, n, "%lu:%02lu", (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60));
}

static void ui_task(void *arg) {
    (void)arg;
    page_t pg = PG_HOME;
    int item = 0;
    bool a_prev = false, b_prev = false, dim = false;
    int64_t last_press = now_ms(), last_draw = 0, last_batt = -60000;
    board_batt_t batt = { .pct = -1 };
    static char prev[800];
    char title[24], ln[BOARD_LINES][64], la[24], lb[24];

    for (;;) {
        board_buttons_poll();
        int64_t now = now_ms();
        bool a = board_btn_a_down(), b = board_btn_b_down();
        bool a_hit = a && !a_prev, b_hit = b && !b_prev;
        a_prev = a; b_prev = b;

        int pek = 0;
        static int64_t last_pek;
        if (now - last_pek > 100) { last_pek = now; pek = board_power_key(); }
        if (pek == 2) {
            craw_serial_print("\r\n[UI] power key held — powering off\r\n");
            board_screen_t s = { .title = "POWER OFF", .tone = BOARD_TONE_ALERT,
                                 .lines = { "press the power key", "(bottom edge) to turn on" } };
            board_brightness(bright_pct);
            board_draw(&s);
            vTaskDelay(pdMS_TO_TICKS(800));
            board_power_off();
        }

        if (a_hit || b_hit || pek) {
            last_press = now;
            if (dim) {                       // first press only wakes the screen
                dim = false;
                board_brightness(bright_pct);
                a_hit = b_hit = false;
                pek = 0;
            }
        } else if (!dim && now - last_press > DIM_AFTER_MS) {
            dim = true;
            board_brightness(DIM_PCT);
            pg = PG_HOME;
        }
        static net_state_t prev_ns = NET_OFFLINE;
        net_state_t ns_now = net_portal_state();
        if (ns_now != prev_ns) {
            if (ns_now == NET_PORTAL || ns_now == NET_SETUP || prev_ns == NET_RELAY || prev_ns == NET_SETUP)
                s_wake = true;
            prev_ns = ns_now;
        }
        if (s_wake) {                        // presence changed: show it
            s_wake = false;
            last_press = now;
            if (dim) { dim = false; board_brightness(bright_pct); }
            if (pg != PG_CONFIRM) pg = PG_HOME;
        }
        if (pg >= PG_MENU && now - last_press > MENU_TIMEOUT_MS) pg = PG_HOME;
        if (pek == 1) pg = PG_HOME;          // short power-key press = home

        switch (pg) {
        case PG_HOME: {
            // While the WiFi wants a sign-in, B is the way through it.
            net_state_t ns = net_portal_state();
            if (a_hit) pg = PG_STATS;
            if (b_hit && ns == NET_PORTAL) net_portal_relay(true);
            else if (b_hit && (ns == NET_RELAY || ns == NET_SETUP)) net_portal_relay(false);
            else if (b_hit) { pg = PG_MENU; item = 0; }
            break;
        }
        case PG_STATS:
            if (a_hit) pg = PG_HOME;
            if (b_hit) { pg = PG_MENU; item = 0; }
            break;
        case PG_MENU:
            if (b_hit) item = (item + 1) % MI_N;
            if (a_hit && item == MI_LOGIN) { net_portal_relay(true); pg = PG_HOME; }
            else if (a_hit && item == MI_SETUP) { net_portal_setup(); pg = PG_HOME; }
            else if (a_hit) pg = item == MI_EXIT ? PG_HOME : item == MI_INFO ? PG_INFO : PG_CONFIRM;
            break;
        case PG_CONFIRM:
            if (a_hit) {
                stats_reset_req(menu_mask(item));
                pg = PG_STATS;
            }
            if (b_hit) pg = PG_MENU;
            break;
        case PG_INFO:
            if (a_hit) pg = PG_HOME;
            if (b_hit) pg = PG_MENU;
            break;
        }

        if (now - last_batt > 30000) { board_batt_read(&batt); s_batt = batt; last_batt = now; }

        if (now - last_draw < 250 && !a_hit && !b_hit) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        last_draw = now;

        LOCK();
        presence_t p = s_pres;
        stats_t st = s_stats;
        int64_t changed = s_changed_ms;
        recent_advance(&s_recent, now);
        uint32_t recent_ms = recent_total_ms(&s_recent);
        bool quiet = s_quiet;
        int64_t quiet_for = quiet_s(now);
        UNLOCK();

        board_screen_t s = { .title = title };
        title[0] = la[0] = lb[0] = 0;
        for (int i = 0; i < BOARD_LINES; i++) { ln[i][0] = 0; s.lines[i] = ln[i]; }
        s.btn_a = la; s.btn_b = lb;
        char clk[16], hm[16];
        struct tm lt;
        bool have_clock = local_now(&lt);

        net_state_t ns = net_portal_state();
        if (pg == PG_HOME && ns == NET_SETUP) {
            snprintf(title, sizeof(title), "WIFI SETUP");
            s.tone = BOARD_TONE_WARN;
            snprintf(ln[0], sizeof(ln[0]), "1. on your phone, join WiFi:");
            snprintf(ln[1], sizeof(ln[1]), "   %s", net_portal_ap_ssid());
            snprintf(ln[2], sizeof(ln[2]), "   password %s", net_portal_ap_pass());
            snprintf(ln[3], sizeof(ln[3]), "2. pick WiFi (page: 10.77.0.1)");
            snprintf(ln[4], sizeof(ln[4]), net_portal_ap_clients() ? "phone connected - waiting..."
                                                                   : "(closes after 10 min)");
            snprintf(la, sizeof(la), "A: stats");
            snprintf(lb, sizeof(lb), "B: cancel");
        } else if (pg == PG_HOME && ns == NET_RELAY) {
            snprintf(title, sizeof(title), "PHONE LOGIN");
            s.tone = BOARD_TONE_WARN;
            snprintf(ln[0], sizeof(ln[0]), "1. on your phone, join WiFi:");
            snprintf(ln[1], sizeof(ln[1]), "   %s", net_portal_ap_ssid());
            snprintf(ln[2], sizeof(ln[2]), "   password %s", net_portal_ap_pass());
            snprintf(ln[3], sizeof(ln[3]), "2. tap OK on the sign-in page");
            snprintf(ln[4], sizeof(ln[4]), net_portal_ap_clients() ? "phone connected - waiting..."
                                                                   : "(closes once online)");
            snprintf(la, sizeof(la), "A: stats");
            snprintf(lb, sizeof(lb), "B: stop");
        } else if (pg == PG_HOME && ns == NET_PORTAL) {
            s.icon = true;
            snprintf(title, sizeof(title), "LOGIN NEEDED");
            s.tone = BOARD_TONE_WARN;
            snprintf(ln[0], sizeof(ln[0]), "WiFi has a sign-in page");
            snprintf(ln[1], sizeof(ln[1]), "%.28s", net_portal_detail());
            snprintf(ln[2], sizeof(ln[2]), "press B, then use a phone");
            snprintf(ln[3], sizeof(ln[3]), "(still counting entries)");
            snprintf(la, sizeof(la), "A: stats");
            snprintf(lb, sizeof(lb), "B: phone login");
        } else switch (pg) {
        case PG_HOME: {
            s.icon = true;
            s.motion = p.motion;
            int ago = (int)((now - changed) / 1000);
            if (s_toast_until > now) {
                snprintf(title, sizeof(title), "%s", s_toast);
                s.tone = BOARD_TONE_WARN;
            } else if (quiet && p.state == PRES_CLEAR) {
                snprintf(title, sizeof(title), "CHECK SENSOR");
                s.tone = BOARD_TONE_WARN;
            } else if (p.state == PRES_WARMUP) {
                snprintf(title, sizeof(title), "WARMING UP");
                s.tone = BOARD_TONE_WARN;
            } else if (p.state == PRES_PRESENT) {
                snprintf(title, sizeof(title), p.motion ? "ACTIVE" : "PRESENT");
                s.tone = BOARD_TONE_ALERT;
            } else {
                snprintf(title, sizeof(title), "CLEAR");
                s.tone = BOARD_TONE_OK;
            }
            if (p.state == PRES_WARMUP)
                snprintf(ln[0], sizeof(ln[0]), "sensor settling %ds",
                         (int)(WARMUP_S - (now - p.t_start_ms) / 1000));
            else {
                char d[16];
                fmt_dur(d, sizeof(d), ago);
                if (p.state == PRES_PRESENT)
                    snprintf(ln[0], sizeof(ln[0]), ago < 60 ? "entered %s" : "entered %s ago", d);
                else
                    snprintf(ln[0], sizeof(ln[0]), ago < 60 ? "clear %s" : "clear for %s", d);
            }
            snprintf(ln[1], sizeof(ln[1]), "in: hour %lu  today %lu",
                     (unsigned long)st.entries[ST_HOUR], (unsigned long)st.entries[ST_DAY]);
            snprintf(ln[2], sizeof(ln[2]), "%s", net_line());
            if (have_clock) strftime(clk, sizeof(clk), "%a %H:%M", &lt);
            else snprintf(clk, sizeof(clk), "clock not set");
            snprintf(ln[3], sizeof(ln[3]), "%s  %s%d%%", clk, batt.usb ? "usb " : "batt ", batt.pct);
            // The hold timer restarts at every sighting; show where it stands.
            if (p.state == PRES_PRESENT && !p.motion) {
                int left = (int)p.hold_s - (int)((now - p.t_motion_ms) / 1000);
                if (left < 60) snprintf(ln[4], sizeof(ln[4]), "clear in <1m if still");
                else snprintf(ln[4], sizeof(ln[4]), "clear in %dm if still", (left + 59) / 60);
            } else if (quiet) {
                snprintf(ln[4], sizeof(ln[4]), "no motion %lldh: wiring ok?",
                         (long long)(quiet_for / 3600));
            } else if (p.state != PRES_WARMUP) {
                snprintf(ln[4], sizeof(ln[4]), "last 5 min: %lus active",
                         (unsigned long)((recent_ms + 500) / 1000));
            }
            snprintf(la, sizeof(la), "A: stats");
            snprintf(lb, sizeof(lb), "B: menu");
            break;
        }
        case PG_STATS:
            s.title = NULL;
            s.lines_big = true;
            snprintf(ln[0], sizeof(ln[0]), "%-5s %4s %7s", "", "in", "h:mm");
            for (int i = 0; i < ST_N; i++) {
                fmt_hm(hm, sizeof(hm), st.occ_s[i]);
                snprintf(ln[i + 1], sizeof(ln[i + 1]), "%-5s %4lu %7s", stats_period_name(i),
                         (unsigned long)st.entries[i], hm);
            }
            snprintf(la, sizeof(la), "A: home");
            snprintf(lb, sizeof(lb), "B: menu");
            break;
        case PG_MENU:
            snprintf(title, sizeof(title), "%s", MENU[item]);
            s.title_big = strlen(MENU[item]) <= 12;
            s.tone = item <= MI_ALL ? BOARD_TONE_WARN : BOARD_TONE_NORMAL;
            snprintf(ln[0], sizeof(ln[0]), "menu %d of %d", item + 1, MI_N);
            if (item < MI_ALL) {
                fmt_hm(hm, sizeof(hm), st.occ_s[item]);
                snprintf(ln[1], sizeof(ln[1]), "this %s: %lu in, %s occupied",
                         stats_period_name(item), (unsigned long)st.entries[item], hm);
            } else if (item == MI_ALL) {
                snprintf(ln[1], sizeof(ln[1]), "hour, day, week and month");
            } else if (item == MI_LOGIN) {
                snprintf(ln[1], sizeof(ln[1]), "sign in to WiFi from a phone");
                snprintf(ln[2], sizeof(ln[2]), "(no WiFi: opens WiFi setup)");
            } else if (item == MI_SETUP) {
                snprintf(ln[1], sizeof(ln[1]), "pick WiFi + password");
                snprintf(ln[2], sizeof(ln[2]), "from a phone");
            }
            snprintf(lb, sizeof(lb), "B: next");
            snprintf(la, sizeof(la), item == MI_EXIT ? "A: close menu" : "A: select");
            break;
        case PG_CONFIRM:
            snprintf(title, sizeof(title), "%s?", MENU[item]);
            s.tone = BOARD_TONE_ALERT;
            snprintf(ln[0], sizeof(ln[0]), "counts and occupied time");
            snprintf(ln[1], sizeof(ln[1]), "go back to zero");
            snprintf(ln[2], sizeof(ln[2]), "(subscribers get a reset event)");
            snprintf(la, sizeof(la), "A: yes, reset");
            snprintf(lb, sizeof(lb), "B: no");
            break;
        case PG_INFO: {
            char ip[20] = "-";
            if (craw_wifi_is_connected()) craw_wifi_get_ip_str(ip, sizeof(ip));
            const char *host = strstr(mqtt_broker, "://");
            host = host ? host + 3 : mqtt_broker;
            snprintf(title, sizeof(title), "DEVICE INFO");
            snprintf(ln[0], sizeof(ln[0]), "%s  fw %s", hostname, SEN_FW_VERSION);
            snprintf(ln[1], sizeof(ln[1]), "wifi %.20s  ip %s", wifi_ssid[0] ? wifi_ssid : "(none)", ip);
            snprintf(ln[2], sizeof(ln[2]), "mqtt %.40s %s", host, craw_mqtt_is_connected() ? "ok" : "--");
            snprintf(ln[3], sizeof(ln[3]), "%.60s/#", topic_base);
            snprintf(ln[4], sizeof(ln[4]), "hold %lus  repeat %lus",
                     (unsigned long)p.hold_s, (unsigned long)p.repeat_s);
            snprintf(la, sizeof(la), "A: home");
            snprintf(lb, sizeof(lb), "B: menu");
            break;
        }
        }

        char cur[sizeof(prev)];
        snprintf(cur, sizeof(cur), "%d|%s|%d|%d|%s|%s|%s|%s|%s|%s|%s", pg, s.title ? s.title : "",
                 s.tone, s.motion, ln[0], ln[1], ln[2], ln[3], ln[4], la, lb);
        if (strcmp(cur, prev)) {
            strcpy(prev, cur);
            board_draw(&s);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------- Forth words ---------- */
static void w_sen_stat(void) {
    char clk[32] = "not synced", hm[16];
    struct tm lt;
    if (local_now(&lt)) strftime(clk, sizeof(clk), "%Y-%m-%d %H:%M:%S %Z", &lt);
    board_batt_t b = s_batt;
    LOCK();
    presence_t p = s_pres;
    stats_t st = s_stats;
    UNLOCK();
    craw_serial_printf("\r\nfw:       %s  (%s)\r\n", SEN_FW_VERSION, hostname);
    craw_serial_printf("clock:    %s  TZ=%s\r\n", clk, tz);
    craw_serial_printf("pir:      %s  motion=%d raw=%d  hold %lus repeat %lus\r\n",
                       presence_state_name(p.state), p.motion, board_pir_raw(),
                       (unsigned long)p.hold_s, (unsigned long)p.repeat_s);
    LOCK();
    recent_advance(&s_recent, now_ms());
    uint32_t r_ms = recent_total_ms(&s_recent);
    int32_t hkey = s_hist.key;
    uint32_t h24 = 0;
    for (int i = 0; i < HIST_BINS; i++) h24 += s_hist.occ_s[i];
    UNLOCK();
    craw_serial_printf("quiet:    %s (warn after %luh without motion)\r\n",
                       s_quiet ? "CHECK SENSOR" : "ok", (unsigned long)s_quiet_h);
    craw_serial_printf("activity: last 5 min %lu.%lus active  last 24 h %lum occupied  (bin %s)\r\n",
                       (unsigned long)(r_ms / 1000), (unsigned long)(r_ms % 1000 / 100),
                       (unsigned long)(h24 / 60), hkey ? "aligned" : "clock unknown");
    craw_serial_printf("net:      %s\r\n", net_line());
    craw_serial_printf("mqtt:     %s  base %s  queued %u dropped %lu\r\n", mqtt_broker, topic_base,
                       (unsigned)uxQueueMessagesWaiting(s_txq), (unsigned long)s_tx_dropped);
    for (int i = 0; i < ST_N; i++) {
        char label[24];
        fmt_hm(hm, sizeof(hm), st.occ_s[i]);
        stats_label(i, st.key[i], label, sizeof(label));
        craw_serial_printf("%-6s    %4lu in  %8s occupied  (%s)\r\n", stats_period_name(i),
                           (unsigned long)st.entries[i], hm, st.key[i] ? label : "period unknown");
    }
    craw_serial_printf("batt:     %d mV %d%%%s  heap %u\r\n", b.mv, b.pct, b.usb ? " usb" : "",
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void w_reset_hour(void)  { stats_reset_req(1u << ST_HOUR); }
static void w_reset_day(void)   { stats_reset_req(1u << ST_DAY); }
static void w_reset_week(void)  { stats_reset_req(1u << ST_WEEK); }
static void w_reset_month(void) { stats_reset_req(1u << ST_MONTH); }
static void w_reset_all(void)   { stats_reset_req(ST_ALL); }

/* ( secs -- ) */
static void w_pir_hold(void) {
    int v = (int)forth_pop();
    if (v < 10) v = 10;
    LOCK(); s_pres.hold_s = v; UNLOCK();
    nvs_set_u("hold", v);
    craw_serial_printf("\r\nhold = %d s (no motion this long = clear)\r\n", v);
}
static void w_pir_repeat(void) {
    int v = (int)forth_pop();
    if (v < 10) v = 10;
    LOCK(); s_pres.repeat_s = v; UNLOCK();
    nvs_set_u("repeat", v);
    craw_serial_printf("\r\nrepeat = %d s (re-ping while present)\r\n", v);
}
static void w_net_check(void) {
    net_portal_check();
    craw_serial_print("\r\nchecking the internet (see net-stat)\r\n");
}
static void w_net_login(void) {
    net_portal_relay(true);
    craw_serial_print("\r\nopening the phone login network — details on the Stick's screen\r\n");
}
static void w_net_check_url(void) {
    craw_serial_printf("\r\nCheck URL: %s\r\nNew (Enter keeps, '-' = default): ", net_portal_check_url());
    char buf[128] = {0};
    craw_serial_read_line(buf, sizeof(buf), true, true);
    if (!buf[0]) { craw_serial_print("unchanged\r\n"); return; }
    net_portal_set_check_url(strcmp(buf, "-") ? buf : NULL);
    craw_serial_printf("check URL = %s (until reboot)\r\n", net_portal_check_url());
}
static void w_net_setup(void) {
    net_portal_setup();
    craw_serial_print("\r\nopening the WiFi setup network — details on the Stick's screen\r\n");
}
static void w_net_stop(void) { net_portal_relay(false); craw_serial_print("\r\nlogin/setup network closed\r\n"); }
static void w_net_stat(void) {
    craw_serial_printf("\r\nnet:   %s — %s\r\n", net_portal_state_name(net_portal_state()), net_portal_detail());
    if (net_portal_state() == NET_RELAY || net_portal_state() == NET_SETUP)
        craw_serial_printf("join:  %s  password %s  phones %d\r\n", net_portal_ap_ssid(),
                           net_portal_ap_pass(), net_portal_ap_clients());
    craw_serial_printf("mqtt:  %s (%s)\r\n", mqtt_active, craw_mqtt_is_connected() ? "connected" : "down");
}
static void w_mqtt_alt(void) {
    craw_serial_printf("\r\nAlternate broker: %s\r\nNew (Enter keeps, '-' disables): ", mqtt_alt[0] ? mqtt_alt : "(none)");
    char buf[128] = {0};
    craw_serial_read_line(buf, sizeof(buf), true, true);
    if (!buf[0]) { craw_serial_print("unchanged\r\n"); return; }
    snprintf(mqtt_alt, sizeof(mqtt_alt), "%s", strcmp(buf, "-") ? buf : "");
    nvs_set_s("mqtt_alt", mqtt_alt);
    craw_serial_printf("alternate = %s\r\n", mqtt_alt[0] ? mqtt_alt : "(none)");
}

/* ( hours -- ) warn after this long without any motion */
static void w_pir_quiet(void) {
    int v = (int)forth_pop();
    if (v < 1) v = 1;
    if (v > 168) v = 168;
    s_quiet_h = v;
    nvs_set_u("quiet_h", v);
    craw_serial_printf("\r\ncheck-sensor warning after %d h without motion\r\n", v);
}

/* ( secs -- ) fake motion, for testing without the sensor */
static void w_pir_sim(void) {
    int v = (int)forth_pop();
    s_sim_until = now_ms() + (int64_t)v * 1000;
    craw_serial_printf("\r\nsimulated motion for %d s\r\n", v);
}

static void w_tz(void) {
    craw_serial_printf("\r\nTZ now: %s\r\n", tz);
    craw_serial_print("POSIX TZ (e.g. PST8PDT,M3.2.0,M11.1.0  JST-9  UTC0; Enter keeps): ");
    char buf[64] = {0};
    craw_serial_read_line(buf, sizeof(buf), true, true);
    if (!buf[0]) { craw_serial_print("unchanged\r\n"); return; }
    snprintf(tz, sizeof(tz), "%s", buf);
    nvs_set_s("tz", tz);
    tz_apply();
    craw_serial_printf("TZ = %s\r\n", tz);
}

static void w_wifi_ssid(void) {
    craw_serial_print("\r\nSSID: ");
    craw_serial_read_line(wifi_ssid, sizeof(wifi_ssid), true, false);
    craw_serial_printf("SSID set to '%s' — now wifi-pass, then wifi-connect\r\n", wifi_ssid);
}
static void w_wifi_pass(void) {
    craw_serial_print("\r\nPassword (Enter for open): ");
    craw_serial_read_line(wifi_pass, sizeof(wifi_pass), false, true);
    craw_serial_print(wifi_pass[0] ? "Password set.\r\n" : "No password (open network)\r\n");
}
static void w_wifi_connect(void) {
    if (!wifi_ssid[0]) { craw_serial_print("\r\nNo SSID set. Use wifi-ssid first.\r\n"); return; }
    craw_nvs_save_wifi_creds(wifi_ssid, wifi_pass);
    net_portal_saved_ssid(wifi_ssid);
    craw_serial_printf("\r\nSaved. Connecting to '%s'...\r\n", wifi_ssid);
    craw_wifi_connect(wifi_ssid, wifi_pass);
}
static void w_wifi_status(void) {
    char ip[20] = "N/A";
    if (craw_wifi_is_connected()) craw_wifi_get_ip_str(ip, sizeof(ip));
    craw_serial_printf("\r\nSSID: '%s'  connected: %s  IP: %s\r\n", wifi_ssid,
                       craw_wifi_is_connected() ? "yes" : "no", ip);
}
static void w_wifi_clear(void) {
    craw_nvs_clear_wifi_creds(wifi_ssid, wifi_pass);
    net_portal_saved_ssid("");
    craw_wifi_disconnect();
    craw_serial_print("\r\nWiFi credentials cleared.\r\n");
}

static void w_mqtt_broker(void) {
    craw_serial_printf("\r\nCurrent: %s\r\nNew broker URI (Enter to keep): ", mqtt_broker);
    char buf[128] = {0};
    craw_serial_read_line(buf, sizeof(buf), true, true);
    if (!buf[0]) { craw_serial_print("unchanged\r\n"); return; }
    snprintf(mqtt_broker, sizeof(mqtt_broker), "%s", buf);
    craw_nvs_save_mqtt_broker(mqtt_broker);
    craw_mqtt_stop();
    mqtt_setup(mqtt_broker);
    if (craw_wifi_is_connected()) craw_mqtt_start();
    craw_serial_printf("broker = %s\r\n", mqtt_broker);
}
static void w_mqtt_base(void) {
    craw_serial_printf("\r\nCurrent: %s\r\nNew topic base (Enter to keep): ", topic_base);
    char buf[96] = {0};
    craw_serial_read_line(buf, sizeof(buf), true, true);
    if (!buf[0]) { craw_serial_print("unchanged\r\n"); return; }
    snprintf(topic_base, sizeof(topic_base), "%s", buf);
    nvs_set_s("base", topic_base);
    set_topics();
    craw_mqtt_stop();                     /* the last will names the state topic */
    mqtt_setup(mqtt_active ? mqtt_active : mqtt_broker);
    if (craw_wifi_is_connected()) craw_mqtt_start();
    craw_serial_printf("events -> %s\r\nstate  -> %s (retained)\r\n", topic_event, topic_state);
}
static void w_mqtt_status(void) {
    craw_serial_printf("\r\nbroker: %s  connected: %s  (alternate: %s)\r\nevents: %s\r\nstate:  %s (retained)\r\n",
                       mqtt_active, craw_mqtt_is_connected() ? "yes" : "no",
                       mqtt_alt[0] ? mqtt_alt : "none", topic_event, topic_state);
}

static void w_brightness(void) {
    int v = (int)forth_pop();
    bright_pct = v < 5 ? 5 : v > 100 ? 100 : v;
    board_brightness(bright_pct);
    nvs_set_u("bright", bright_pct);
    craw_serial_printf("\r\nbrightness %d%%\r\n", bright_pct);
}
static void w_power_off(void) {
    craw_serial_print("\r\npowering off (power key turns it back on)\r\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    board_power_off();
}

static void register_forth_words(void) {
    forth_register_word("sen-stat",     w_sen_stat);
    forth_register_word("reset-hour",   w_reset_hour);
    forth_register_word("reset-day",    w_reset_day);
    forth_register_word("reset-week",   w_reset_week);
    forth_register_word("reset-month",  w_reset_month);
    forth_register_word("reset-all",    w_reset_all);
    forth_register_word("pir-hold",     w_pir_hold);
    forth_register_word("pir-repeat",   w_pir_repeat);
    forth_register_word("pir-sim",      w_pir_sim);
    forth_register_word("pir-quiet",    w_pir_quiet);
    forth_register_word("net-check",    w_net_check);
    forth_register_word("net-login",    w_net_login);
    forth_register_word("net-stop",     w_net_stop);
    forth_register_word("net-setup",    w_net_setup);
    forth_register_word("net-check-url", w_net_check_url);
    forth_register_word("net-stat",     w_net_stat);
    forth_register_word("mqtt-alt",     w_mqtt_alt);
    forth_register_word("tz",           w_tz);
    forth_register_word("wifi-ssid",    w_wifi_ssid);
    forth_register_word("wifi-pass",    w_wifi_pass);
    forth_register_word("wifi-connect", w_wifi_connect);
    forth_register_word("wifi-status",  w_wifi_status);
    forth_register_word("wifi-clear",   w_wifi_clear);
    forth_register_word("mqtt-broker",  w_mqtt_broker);
    forth_register_word("mqtt-base",    w_mqtt_base);
    forth_register_word("mqtt-status",  w_mqtt_status);
    forth_register_word("brightness",   w_brightness);
    forth_register_word("power-off",    w_power_off);
}

/* ---------- app_main ---------- */
void app_main(void) {
    craw_serial_init();
    vTaskDelay(pdMS_TO_TICKS(300));

    craw_serial_print("\r\n\r\n====================================\r\n");
    craw_serial_print(  "  MagNET Hive Sentinel IR — PIR presence\r\n");
    craw_serial_printf( "  fw %s  ESPIDFORTH %s  gen %s\r\n", SEN_FW_VERSION,
                        ESPIDFORTH_VERSION_STRING, MAGNET_GEN_STR);
    craw_serial_print(  "  M5StickC Plus + RadioShack 276-0347 PIR on G26\r\n");
    craw_serial_print(  "====================================\r\n");

    /* M5GFX first: it owns I2C port 1 (AXP192) and powers the panel. */
    board_init();
    derive_ids();
    board_screen_t splash = { .title = "SENTINEL", .icon = true, .lines = { "booting..." } };
    board_draw(&splash);

    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    craw_nvs_profiles_load();
    craw_nvs_load_mqtt_broker(mqtt_broker, sizeof(mqtt_broker));
    nvs_get_s("mqtt_alt", mqtt_alt, sizeof(mqtt_alt));
    snprintf(topic_base, sizeof(topic_base), "iotj/magnet/sentinel/%s", mac4);
    nvs_get_s("base", topic_base, sizeof(topic_base));
    set_topics();
    nvs_get_s("tz", tz, sizeof(tz));
    tz_apply();
    bright_pct = (int)nvs_get_u("bright", DEF_BRIGHT);
    board_brightness(bright_pct);

    s_lock = xSemaphoreCreateMutex();
    s_txq = xQueueCreate(TX_DEPTH, sizeof(msg_t));
    stats_load();
    hist_load();
    recent_init(&s_recent, now_ms());
    s_quiet_h = nvs_get_u("quiet_h", DEF_QUIET_H);
    presence_init(&s_pres, now_ms(), nvs_get_u("hold", DEF_HOLD_S),
                  nvs_get_u("repeat", DEF_REPEAT_S), WARMUP_S);

    /* Forth before WiFi, or the dictionary heap starves. */
    forth_init(FORTH_HEAP_SIZE);
    register_forth_words();

    craw_wifi_init(hostname, on_wifi_event, NULL);
    mqtt_setup(mqtt_broker);
    char ap_name[24];
    snprintf(ap_name, sizeof(ap_name), "Sentinel-%s", mac4);
    net_portal_init(ap_name);

    xTaskCreate(sensor_task, "pir", 4096, NULL, 4, NULL);
    xTaskCreate(tx_task, "mqtt_tx", 4096, NULL, 3, NULL);
    xTaskCreate(ui_task, "ui", 6144, NULL, 2, NULL);

    craw_serial_printf("node %s  broker %s\r\nevents %s\r\nstate  %s (retained)\r\n",
                       hostname, mqtt_broker, topic_event, topic_state);
    bool have_wifi = craw_nvs_load_wifi_creds(wifi_ssid, wifi_pass);
    net_portal_on_wifi_saved(wifi_saved, wifi_ssid);
    if (have_wifi) {
        craw_serial_printf("WiFi '%s' — connecting...\r\n", wifi_ssid);
        craw_wifi_connect(wifi_ssid, wifi_pass);
    } else {
        craw_serial_print("No WiFi stored: wifi-ssid, wifi-pass, wifi-connect\r\n");
    }

    craw_serial_print("\r\nForth commands:\r\n");
    craw_serial_print("  sen-stat                     -- presence, stats, network, battery\r\n");
    craw_serial_print("  reset-hour/-day/-week/-month/-all\r\n");
    craw_serial_print("  N pir-hold / N pir-repeat    -- clear after N s still / re-ping every N s\r\n");
    craw_serial_print("  N pir-sim                    -- fake motion for N s\r\n");
    craw_serial_print("  N pir-quiet                  -- warn \"check sensor\" after N h of no motion\r\n");
    craw_serial_print("  tz                           -- set POSIX time zone (stats boundaries)\r\n");
    craw_serial_print("  wifi-ssid / wifi-pass / wifi-connect / wifi-status / wifi-clear\r\n");
    craw_serial_print("  mqtt-broker / mqtt-alt / mqtt-base / mqtt-status\r\n");
    craw_serial_print("  net-stat / net-check / net-login / net-stop -- sign-in (captive portal) WiFi\r\n");
    craw_serial_print("  net-setup                    -- pick WiFi from a phone (Sentinel-xxxx-setup)\r\n");
    craw_serial_print("  N brightness / power-off\r\n");
    craw_serial_print("  buttons: A (front) stats/select, B (side) menu/next; power key hold = off\r\n\r\n");

    forth_repl(craw_serial_getchar, craw_serial_putchar);
    forth_deinit();
}
