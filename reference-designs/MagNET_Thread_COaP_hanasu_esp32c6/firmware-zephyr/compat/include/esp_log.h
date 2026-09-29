/* esp_log.h — compiled to nothing on Zephyr. The console UART IS the HCP link;
 * a stray log line there breaks §11.3 framing (the same bug IDF's INFO logs
 * caused, fixed there with CONFIG_LOG_DEFAULT_LEVEL_WARN). */
#ifndef MN_COMPAT_ESP_LOG_H
#define MN_COMPAT_ESP_LOG_H
#define ESP_LOG_SINK(tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOGE ESP_LOG_SINK
#define ESP_LOGW ESP_LOG_SINK
#define ESP_LOGI ESP_LOG_SINK
#define ESP_LOGD ESP_LOG_SINK
#define ESP_LOGV ESP_LOG_SINK
#endif
