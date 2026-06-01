/*
 * EspJanet - Janet Language REPL on ESP-IDF
 * Phase 0 of MagNET Hive AI prototype
 *
 * Boots Janet interpreter and provides UART REPL.
 * Reports memory stats at startup.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "janet.h"

static const char *TAG = "espjanet";

static void print_memory_stats(const char *label) {
    printf("\n=== Memory Stats: %s ===\n", label);
    printf("  Free heap (internal): %lu bytes\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("  Largest free block:   %lu bytes\n",
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("  Min free ever:        %lu bytes\n",
           (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
#if CONFIG_SPIRAM
    printf("  Free PSRAM:           %lu bytes\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("  Largest PSRAM block:  %lu bytes\n",
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#endif
    printf("===========================\n\n");
}

/* Simple line-based REPL using stdio */
static void janet_repl_task(void *arg) {
    JanetTable *env = janet_core_env(NULL);

    printf("\nEspJanet REPL v0.1 (Janet %d.%d.%d on ESP-IDF)\n",
           JANET_VERSION_MAJOR, JANET_VERSION_MINOR, JANET_VERSION_PATCH);
    printf("Type Janet expressions. Ctrl-C to interrupt.\n\n");

    print_memory_stats("After Janet init");

    char line[512];
    while (1) {
        printf("janet> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0) continue;

        /* Special commands */
        if (strcmp(line, ":quit") == 0 || strcmp(line, ":q") == 0) {
            printf("Goodbye.\n");
            break;
        }
        if (strcmp(line, ":mem") == 0) {
            print_memory_stats("Current");
            continue;
        }
        if (strcmp(line, ":gc") == 0) {
            janet_collect();
            print_memory_stats("After GC");
            continue;
        }

        /* Evaluate Janet expression */
        Janet result;
        int status = janet_dostring(env, line, "repl", &result);
        if (status == 0) {
            /* Print result if not nil */
            if (!janet_checktype(result, JANET_NIL)) {
                const uint8_t *str = janet_to_string(result);
                printf("%s\n", (const char *)str);
            }
        }
        /* janet_dostring prints errors internally */
    }

    vTaskDelete(NULL);
}

void app_main(void) {
    printf("\n\n");
    printf("============================================\n");
    printf("  EspJanet - Janet Language on ESP-IDF\n");
    printf("  Phase 0: MagNET Hive AI Prototype\n");
    printf("============================================\n");

    print_memory_stats("Before Janet init");

    /* Initialize Janet runtime */
    ESP_LOGI(TAG, "Initializing Janet runtime...");
    janet_init();
    ESP_LOGI(TAG, "Janet runtime initialized.");

    print_memory_stats("After Janet init");

    /* Launch REPL on a task with adequate stack */
    xTaskCreate(janet_repl_task, "janet_repl", 16384, NULL, 5, NULL);
}
