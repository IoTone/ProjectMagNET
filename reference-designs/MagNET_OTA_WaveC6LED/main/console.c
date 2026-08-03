/*
 * console.c — the device's line reader.
 *
 * WHY THIS EXISTS RATHER THAN forth_repl(): the ESPIDFORTH stub has no S" ,
 * no PARSE and no WORD, so a Forth word cannot take a string argument. WiFi
 * credentials, a server URL and a dvc_ token are all strings, so provisioning
 * cannot be expressed in this Forth at all.
 *
 * Rather than perform surgery on forth_core.cpp — which the plan explicitly
 * defers until something forces it — this owns the line loop and routes:
 *
 *     set <key> <value>     -> C, straight into NVS
 *     show                  -> C
 *     wifi / ip / forget    -> C
 *     anything else         -> forth_eval()
 *
 * forth_core.h already exports forth_eval(), forth_register_word(),
 * forth_push() and forth_pop(), so this costs no engine changes and the Forth
 * side keeps its full vocabulary.
 */
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "console.h"
#include "forth_core.h"
#include "magnet_cfg.h"
#include "magnet_ui.h"
#include "craw_wifi.h"
#include "magnet_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LINE_MAX 200

static void (*out)(const char *);

static void trim(char *s) {
    char *p = s + strlen(s);
    while (p > s && (p[-1] == '\r' || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\t')) *--p = '\0';
}

/* Secrets are never echoed back in full. `show` is the command you run when
 * something is wrong, quite possibly while screen-sharing. */
static void show_masked(const char *label, const char *key) {
    char v[CFG_MAX];
    char line[CFG_MAX + 64];
    if (!cfg_get(key, v, sizeof v)) {
        snprintf(line, sizeof line, "  %-12s (unset)\r\n", label);
    } else {
        size_t n = strlen(v);
        snprintf(line, sizeof line, "  %-12s %.4s%s (%u chars)\r\n",
                 label, v, n > 4 ? "..." : "", (unsigned)n);
    }
    out(line);
}

static void cmd_show(void) {
    char v[CFG_MAX], line[CFG_MAX + 64];
    out("config:\r\n");
    if (cfg_get(CFG_WIFI_SSID, v, sizeof v))
        snprintf(line, sizeof line, "  %-12s %s\r\n", "wifi_ssid", v);
    else
        snprintf(line, sizeof line, "  %-12s (unset)\r\n", "wifi_ssid");
    out(line);
    show_masked("wifi_pass", CFG_WIFI_PASS);
    if (cfg_get(CFG_SERVER_URL, v, sizeof v))
        snprintf(line, sizeof line, "  %-12s %s\r\n", "server_url", v);
    else
        snprintf(line, sizeof line, "  %-12s (unset)\r\n", "server_url");
    out(line);
    show_masked("dev_token", CFG_DEV_TOKEN);
    snprintf(line, sizeof line, "  provisioned: %s\r\n",
             cfg_provisioned() ? "yes" : "no");
    out(line);
    if (craw_wifi_is_connected()) {
        char ip[32];
        if (craw_wifi_get_ip_str(ip, sizeof ip)) {
            snprintf(line, sizeof line, "  ip:          %s\r\n", ip);
            out(line);
        }
    } else {
        out("  ip:          (not connected)\r\n");
    }
}

static void cmd_help(void) {
    out("commands (anything else goes to Forth):\r\n"
        "  set <key> <value>   wifi_ssid | wifi_pass | server_url | dev_token\r\n"
        "  show                current config (secrets masked)\r\n"
        "  wifi                connect using the stored credentials\r\n"
        "  checkin             POST /api/devices/check-in now\r\n"
        "  forget <key>        erase one key\r\n"
        "  help                this\r\n");
}

void console_run(int (*getch)(void), void (*putch)(int), void (*print)(const char *)) {
    char line[LINE_MAX];
    int n = 0;
    out = print;
    out("\r\ntype 'help' for device commands, or Forth directly.\r\nok> ");

    for (;;) {
        int c = getch();
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (c == '\r' || c == '\n') {
            putch('\r'); putch('\n');
            line[n] = '\0';
            n = 0;
            trim(line);
            if (line[0] == '\0') { out("ok> "); continue; }

            if (strncmp(line, "set ", 4) == 0) {
                char *key = line + 4;
                while (*key == ' ') key++;
                char *val = strchr(key, ' ');
                if (!val) { out("usage: set <key> <value>\r\n"); }
                else {
                    *val++ = '\0';
                    while (*val == ' ') val++;
                    if (cfg_set(key, val) == ESP_OK) {
                        /* Report the LENGTH, never the value: `set dev_token ...`
                         * is the one command whose echo would leak a credential
                         * into a scrollback or a screen recording. */
                        char m[CFG_MAX + 48];
                        snprintf(m, sizeof m, "%.*s set (%u chars)\r\n",
                                 (int)CFG_MAX, key, (unsigned)strlen(val));
                        out(m);
                    } else out("write failed\r\n");
                }
            } else if (strcmp(line, "show") == 0) {
                cmd_show();
            } else if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
                cmd_help();
            } else if (strncmp(line, "forget ", 7) == 0) {
                cfg_erase(line + 7);
                out("erased\r\n");
            } else if (strcmp(line, "checkin") == 0) {
                /* Hand the work to the poll task rather than doing it here.
                 * One owner of check-ins, so there is no second HTTP client to
                 * reason about. */
                char m[128];
                out("check-in requested...\r\n");
                ota_request_checkin();
                vTaskDelay(pdMS_TO_TICKS(4000));
                snprintf(m, sizeof m, "  %s\r\n", ota_last_status());
                out(m);
                const ota_release_t *r = ota_pending();
                if (r) {
                    snprintf(m, sizeof m, "  release %ld  version %s\r\n",
                             r->release_id, r->version);
                    out(m);
                }
            } else if (strcmp(line, "wifi") == 0) {
                char ssid[CFG_MAX], pass[CFG_MAX];
                if (!cfg_get(CFG_WIFI_SSID, ssid, sizeof ssid)) {
                    out("no wifi_ssid set\r\n");
                } else {
                    cfg_get(CFG_WIFI_PASS, pass, sizeof pass);
                    out("connecting...\r\n");
                    craw_wifi_connect(ssid, pass);
                }
            } else {
                forth_eval(line);
            }
            out("ok> ");
        } else if (c == 8 || c == 127) {          /* backspace */
            if (n > 0) { n--; putch(8); putch(' '); putch(8); }
        } else if (n < LINE_MAX - 1 && c >= 32 && c < 127) {
            line[n++] = (char)c;
            putch(c);                              /* echo */
        }
    }
}
