#include "craw_mr60bha2.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "craw_mr60";

/* ─── Tiny Frame Interface constants (per Seeed Arduino mmWave lib) ───── */

#define SOF_BYTE             0x01u
#define HEADER_SIZE          8u    // SOF + ID(2) + LEN(2) + TYPE(2) + HEAD_CKSUM
#define DATA_CKSUM_SIZE      1u
#define MAX_FRAME_SIZE       512u
#define UART_RX_BUFFER       1024u
#define READ_BLOCK_MS        50

/* Frame TYPE identifiers we consume */
#define TYPE_HB_PHASE        0x0A13
#define TYPE_BREATH_RATE     0x0A14
#define TYPE_HEART_RATE      0x0A15
#define TYPE_HB_DISTANCE     0x0A16
#define TYPE_TARGETS_INFO    0x0A04
#define TYPE_TARGETS_PC      0x0A08
#define TYPE_HUMAN_DETECT    0x0F09
#define TYPE_FIRMWARE        0xFFFF

/* History sampling cadence */
#define HISTORY_INTERVAL_US  (60LL * 1000LL * 1000LL)  // 1 minute

/* ─── Module state ────────────────────────────────────────────────────── */

static SemaphoreHandle_t  s_state_mtx;
static craw_mr60_state_t  s_state;

typedef struct {
    uint64_t t_ms[CRAW_MR60_HISTORY_LEN];
    float    v[CRAW_MR60_HISTORY_LEN];
    int      head;
    int      count;
    int64_t  last_push_us;
} ring_t;

static ring_t s_hr_hist;
static ring_t s_rr_hist;

/* Phase waveform — separate ring with three coupled channels. */
typedef struct {
    uint64_t t_ms;
    float    heart;
    float    breath;
    float    total;
} phase_sample_t;

static phase_sample_t s_phase_hist[CRAW_MR60_PHASE_HISTORY_LEN];
static int            s_phase_head = 0;
static int            s_phase_count = 0;

/* Self-test diagnostics. Updated under s_state_mtx. */
static craw_mr60_diagnostics_t s_diag = {0};

static uart_port_t s_port = -1;
static TaskHandle_t s_task = NULL;
static volatile bool s_run = false;

/* ─── Frame helpers ───────────────────────────────────────────────────── */

/* Big-endian u16 — used for LEN and TYPE fields in the frame header. */
static inline uint16_t rd_u16_be(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

/* Little-endian payload values (ESP32 native order). */
static inline uint32_t rd_u32_le(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int32_t rd_i32_le(const uint8_t *p) {
    return (int32_t)rd_u32_le(p);
}

static inline float rd_float_le(const uint8_t *p) {
    union { float f; uint8_t b[4]; } u;
    u.b[0] = p[0]; u.b[1] = p[1]; u.b[2] = p[2]; u.b[3] = p[3];
    return u.f;
}

/* XOR-fold then bitwise-NOT — Seeed's checksum convention. */
static uint8_t calc_xor_chk(const uint8_t *data, size_t len) {
    uint8_t s = 0;
    for (size_t i = 0; i < len; i++) s ^= data[i];
    return (uint8_t)(~s);
}

/* ─── Ring-buffer push (1 sample/min cap) ─────────────────────────────── */

static void push_history(ring_t *r, float v, int64_t now_us) {
    if (r->last_push_us != 0 && (now_us - r->last_push_us) < HISTORY_INTERVAL_US) return;
    r->last_push_us = now_us;
    r->t_ms[r->head] = (uint64_t)(now_us / 1000);
    r->v[r->head]    = v;
    r->head = (r->head + 1) % CRAW_MR60_HISTORY_LEN;
    if (r->count < CRAW_MR60_HISTORY_LEN) r->count++;
}

static size_t copy_history(const ring_t *r, uint64_t *t_ms, float *v, size_t cap) {
    size_t n = (size_t)r->count;
    if (n > cap) n = cap;
    int start = (r->head - r->count + CRAW_MR60_HISTORY_LEN) % CRAW_MR60_HISTORY_LEN;
    for (size_t i = 0; i < n; i++) {
        int idx = (start + (int)i) % CRAW_MR60_HISTORY_LEN;
        t_ms[i] = r->t_ms[idx];
        v[i]    = r->v[idx];
    }
    return n;
}

/* ─── Frame dispatch ──────────────────────────────────────────────────── */

static void handle_frame(uint16_t type, const uint8_t *data, size_t data_len) {
    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    s_state.any_frame_us = now_us;

    switch (type) {
        case TYPE_HEART_RATE:
            if (data_len >= 4) {
                s_state.bpm = rd_float_le(data);
                s_state.bpm_updated_us = now_us;
                if (s_state.bpm > 0.0f) push_history(&s_hr_hist, s_state.bpm, now_us);
            }
            break;
        case TYPE_BREATH_RATE:
            if (data_len >= 4) {
                s_state.rpm = rd_float_le(data);
                s_state.rpm_updated_us = now_us;
                if (s_state.rpm > 0.0f) push_history(&s_rr_hist, s_state.rpm, now_us);
            }
            break;
        case TYPE_HB_PHASE:
            if (data_len >= 12) {
                s_state.total_phase  = rd_float_le(data + 0);
                s_state.breath_phase = rd_float_le(data + 4);
                s_state.heart_phase  = rd_float_le(data + 8);
                /* Push every frame into the waveform ring. */
                phase_sample_t *p = &s_phase_hist[s_phase_head];
                p->t_ms   = (uint64_t)(now_us / 1000);
                p->heart  = s_state.heart_phase;
                p->breath = s_state.breath_phase;
                p->total  = s_state.total_phase;
                s_phase_head = (s_phase_head + 1) % CRAW_MR60_PHASE_HISTORY_LEN;
                if (s_phase_count < CRAW_MR60_PHASE_HISTORY_LEN) s_phase_count++;
            }
            break;
        case TYPE_HB_DISTANCE:
            if (data_len >= 8) {
                s_state.range_flag = rd_u32_le(data);
                s_state.distance_m = rd_float_le(data + 4);
            }
            break;
        case TYPE_HUMAN_DETECT:
            if (data_len >= 1) {
                s_state.present = (data[0] != 0);
                s_state.presence_updated_us = now_us;
            }
            break;
        case TYPE_TARGETS_INFO:
        case TYPE_TARGETS_PC: {
            if (data_len < 4) break;
            uint32_t n = rd_u32_le(data);
            if (n > CRAW_MR60_MAX_TARGETS) n = CRAW_MR60_MAX_TARGETS;
            const uint8_t *p = data + 4;
            const uint8_t *end = data + data_len;
            uint32_t actual = 0;
            for (uint32_t i = 0; i < n && (p + 16) <= end; i++) {
                s_state.targets[i].x_m           = rd_float_le(p + 0);
                s_state.targets[i].y_m           = rd_float_le(p + 4);
                s_state.targets[i].dop_index     = rd_i32_le(p + 8);
                s_state.targets[i].cluster_index = rd_i32_le(p + 12);
                p += 16;
                actual++;
            }
            s_state.target_count = actual;
            s_state.targets_updated_us = now_us;
            break;
        }
        case TYPE_FIRMWARE:
            if (data_len >= 4) s_state.fw_version = rd_u32_le(data);
            break;
        default:
            ESP_LOGD(TAG, "unhandled type 0x%04X len=%u", type, (unsigned)data_len);
            s_diag.unknown_type++;
            break;
    }
    s_diag.frames_valid++;
    if (s_diag.first_frame_us == 0) s_diag.first_frame_us = now_us;
    xSemaphoreGive(s_state_mtx);
}

/* ─── Parse loop ──────────────────────────────────────────────────────── */

static void parse_task(void *arg) {
    (void)arg;
    uint8_t buf[MAX_FRAME_SIZE];
    size_t  buf_len = 0;

    while (s_run) {
        size_t free_space = MAX_FRAME_SIZE - buf_len;
        if (free_space == 0) {
            ESP_LOGW(TAG, "buffer full with no parse progress; resyncing");
            buf_len = 0;
            free_space = MAX_FRAME_SIZE;
        }
        int n = uart_read_bytes(s_port, buf + buf_len, free_space, pdMS_TO_TICKS(READ_BLOCK_MS));
        if (n > 0) {
            buf_len += (size_t)n;
            xSemaphoreTake(s_state_mtx, portMAX_DELAY);
            s_diag.bytes_received += (uint64_t)n;
            xSemaphoreGive(s_state_mtx);
        }
        else if (n < 0) { ESP_LOGW(TAG, "uart_read_bytes err=%d", n); continue; }
        else continue;

        size_t i = 0;
        while (i < buf_len) {
            if (buf[i] != SOF_BYTE) { i++; continue; }
            if (buf_len - i < HEADER_SIZE) break;

            uint16_t data_len = rd_u16_be(buf + i + 3);
            uint16_t type     = rd_u16_be(buf + i + 5);

            if (data_len > MAX_FRAME_SIZE - HEADER_SIZE - DATA_CKSUM_SIZE) {
                /* Implausible — almost certainly desync on a 0x01 byte that wasn't a SOF. */
                xSemaphoreTake(s_state_mtx, portMAX_DELAY);
                s_diag.implausible_len++;
                xSemaphoreGive(s_state_mtx);
                i++;
                continue;
            }
            size_t total = HEADER_SIZE + (size_t)data_len + DATA_CKSUM_SIZE;
            if (buf_len - i < total) break;

            uint8_t header_chk_expected = buf[i + 7];
            uint8_t header_chk_actual   = calc_xor_chk(buf + i, 7);
            if (header_chk_actual != header_chk_expected) {
                ESP_LOGD(TAG, "header cksum mismatch type=0x%04X (resync)", type);
                xSemaphoreTake(s_state_mtx, portMAX_DELAY);
                s_diag.header_cksum_fail++;
                xSemaphoreGive(s_state_mtx);
                i++;
                continue;
            }

            uint8_t data_chk_expected = buf[i + HEADER_SIZE + data_len];
            uint8_t data_chk_actual   = calc_xor_chk(buf + i + HEADER_SIZE, data_len);
            if (data_chk_actual != data_chk_expected) {
                ESP_LOGD(TAG, "data cksum mismatch type=0x%04X len=%u", type, data_len);
                xSemaphoreTake(s_state_mtx, portMAX_DELAY);
                s_diag.data_cksum_fail++;
                xSemaphoreGive(s_state_mtx);
                i++;
                continue;
            }

            handle_frame(type, buf + i + HEADER_SIZE, data_len);
            i += total;
        }

        if (i > 0) {
            if (i < buf_len) memmove(buf, buf + i, buf_len - i);
            buf_len -= i;
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ─── Public API ──────────────────────────────────────────────────────── */

esp_err_t craw_mr60_init(uart_port_t port, int rx_gpio, int tx_gpio) {
    if (s_task) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    s_state_mtx = xSemaphoreCreateMutex();
    if (!s_state_mtx) return ESP_ERR_NO_MEM;

    memset(&s_state, 0, sizeof(s_state));
    memset(&s_hr_hist, 0, sizeof(s_hr_hist));
    memset(&s_rr_hist, 0, sizeof(s_rr_hist));
    memset(&s_phase_hist, 0, sizeof(s_phase_hist));
    memset(&s_diag, 0, sizeof(s_diag));
    s_phase_head = 0;
    s_phase_count = 0;

    uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err;
    if ((err = uart_driver_install(port, UART_RX_BUFFER, 0, 0, NULL, 0)) != ESP_OK) goto fail;
    if ((err = uart_param_config(port, &cfg)) != ESP_OK) goto fail_drv;
    if ((err = uart_set_pin(port, tx_gpio, rx_gpio,
                            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)) != ESP_OK) goto fail_drv;

    s_port = port;
    s_run = true;
    BaseType_t r = xTaskCreate(parse_task, "mr60_parse", 4096, NULL, 5, &s_task);
    if (r != pdPASS) { err = ESP_ERR_NO_MEM; goto fail_drv; }

    ESP_LOGI(TAG, "init ok port=%d rx=%d tx=%d", port, rx_gpio, tx_gpio);
    return ESP_OK;

fail_drv:
    uart_driver_delete(port);
fail:
    vSemaphoreDelete(s_state_mtx);
    s_state_mtx = NULL;
    return err;
}

void craw_mr60_deinit(void) {
    if (!s_run) return;
    s_run = false;
    /* Let the task observe the flag and self-delete. */
    for (int i = 0; i < 20 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_port != (uart_port_t)-1) uart_driver_delete(s_port);
    s_port = -1;
    if (s_state_mtx) { vSemaphoreDelete(s_state_mtx); s_state_mtx = NULL; }
}

void craw_mr60_get_state(craw_mr60_state_t *out) {
    if (!out || !s_state_mtx) { if (out) memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_state_mtx);
}

float craw_mr60_get_bpm(void) {
    float v = 0.0f;
    if (s_state_mtx) {
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        v = s_state.bpm;
        xSemaphoreGive(s_state_mtx);
    }
    return v;
}

float craw_mr60_get_rpm(void) {
    float v = 0.0f;
    if (s_state_mtx) {
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        v = s_state.rpm;
        xSemaphoreGive(s_state_mtx);
    }
    return v;
}

bool craw_mr60_get_presence(void) {
    bool v = false;
    if (s_state_mtx) {
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        v = s_state.present;
        xSemaphoreGive(s_state_mtx);
    }
    return v;
}

size_t craw_mr60_get_targets(craw_mr60_target_t out[CRAW_MR60_MAX_TARGETS]) {
    size_t n = 0;
    if (out && s_state_mtx) {
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        n = s_state.target_count;
        if (n > CRAW_MR60_MAX_TARGETS) n = CRAW_MR60_MAX_TARGETS;
        memcpy(out, s_state.targets, n * sizeof(craw_mr60_target_t));
        xSemaphoreGive(s_state_mtx);
    }
    return n;
}

size_t craw_mr60_get_hr_history(uint64_t *t_ms, float *bpm, size_t cap) {
    size_t n = 0;
    if (t_ms && bpm && s_state_mtx) {
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        n = copy_history(&s_hr_hist, t_ms, bpm, cap);
        xSemaphoreGive(s_state_mtx);
    }
    return n;
}

size_t craw_mr60_get_rr_history(uint64_t *t_ms, float *rpm, size_t cap) {
    size_t n = 0;
    if (t_ms && rpm && s_state_mtx) {
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        n = copy_history(&s_rr_hist, t_ms, rpm, cap);
        xSemaphoreGive(s_state_mtx);
    }
    return n;
}

size_t craw_mr60_get_phase_history(
    uint64_t *t_ms, float *heart, float *breath, float *total, size_t cap)
{
    if (!s_state_mtx) return 0;
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    size_t n = (size_t)s_phase_count;
    if (n > cap) n = cap;
    int start = (s_phase_head - s_phase_count + CRAW_MR60_PHASE_HISTORY_LEN)
                % CRAW_MR60_PHASE_HISTORY_LEN;
    for (size_t i = 0; i < n; i++) {
        int idx = (start + (int)i) % CRAW_MR60_PHASE_HISTORY_LEN;
        const phase_sample_t *p = &s_phase_hist[idx];
        if (t_ms)  t_ms[i]  = p->t_ms;
        if (heart) heart[i] = p->heart;
        if (breath) breath[i] = p->breath;
        if (total) total[i]  = p->total;
    }
    xSemaphoreGive(s_state_mtx);
    return n;
}

void craw_mr60_get_diagnostics(craw_mr60_diagnostics_t *out) {
    if (!out) return;
    if (!s_state_mtx) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    *out = s_diag;
    xSemaphoreGive(s_state_mtx);
}

bool craw_mr60_self_test(uint32_t timeout_ms) {
    if (!s_state_mtx) return false;
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        bool got_one = (s_diag.frames_valid > 0);
        xSemaphoreGive(s_state_mtx);
        if (got_one) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}
