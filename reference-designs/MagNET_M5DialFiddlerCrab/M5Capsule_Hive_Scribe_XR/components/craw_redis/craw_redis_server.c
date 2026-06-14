/*
 * craw_redis_server — RESP2 listener + per-client dispatcher.
 *
 * Reads one command per loop, dispatches via a small table, writes the
 * reply. Per-client task pattern (mirrors craw_hive_ruler) so a stuck
 * peer can't backlog the listener.
 *
 * Inline commands ("PING\r\n") and array form ("*1\r\n$4\r\nPING\r\n")
 * are both accepted to keep telnet and redis-cli equally happy.
 */

#include "craw_redis.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

static const char *TAG = "craw_redis";

/* ---- Server context ---- */

typedef struct {
    craw_redis_config_t  cfg;
    craw_redis_storage_t *storage;
    TaskHandle_t         listen_task;
    int                  listen_sock;
    volatile bool        running;
    volatile int         clients;
    volatile uint64_t    commands_total;
    volatile uint64_t    bytes_in;
    volatile uint64_t    bytes_out;
} ctx_t;

static ctx_t S = { .listen_sock = -1 };

/* ---- Profiles ---- */

void craw_redis_profile_apply(craw_redis_profile_t p, craw_redis_config_t *cfg) {
    if (!cfg) return;
    switch (p) {
        case CRAW_REDIS_PROFILE_LOCAL:
            strcpy(cfg->bind, "127.0.0.1");
            cfg->port = CRAW_REDIS_DEFAULT_PORT;
            break;
        case CRAW_REDIS_PROFILE_LAN:
            strcpy(cfg->bind, "0.0.0.0");
            cfg->port = CRAW_REDIS_DEFAULT_PORT;
            break;
        case CRAW_REDIS_PROFILE_QUIET:
            strcpy(cfg->bind, "0.0.0.0");
            cfg->port = 16379;
            break;
        case CRAW_REDIS_PROFILE_CUSTOM:
            /* Caller supplies values explicitly. */
            break;
    }
}

const char *craw_redis_profile_name(craw_redis_profile_t p) {
    switch (p) {
        case CRAW_REDIS_PROFILE_LOCAL:  return "local";
        case CRAW_REDIS_PROFILE_LAN:    return "lan";
        case CRAW_REDIS_PROFILE_QUIET:  return "quiet";
        case CRAW_REDIS_PROFILE_CUSTOM: return "custom";
    }
    return "?";
}

/* ---- Send helpers ---- */

static int sock_write(int s, const char *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        int w = send(s, p + off, n - off, 0);
        if (w <= 0) return -1;
        off += w;
    }
    S.bytes_out += n;
    return 0;
}

static int reply_simple(int s, const char *line) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "+%s\r\n", line);
    return sock_write(s, buf, n);
}
static int reply_error(int s, const char *line) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "-%s\r\n", line);
    return sock_write(s, buf, n);
}
static int reply_int(int s, long long v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), ":%lld\r\n", v);
    return sock_write(s, buf, n);
}
static int reply_bulk(int s, const char *p, size_t n) {
    char hdr[16];
    int hl = snprintf(hdr, sizeof(hdr), "$%zu\r\n", n);
    if (sock_write(s, hdr, hl) != 0) return -1;
    if (n && sock_write(s, p, n) != 0) return -1;
    return sock_write(s, "\r\n", 2);
}
static int reply_nil(int s) {
    return sock_write(s, "$-1\r\n", 5);
}
static int reply_array_hdr(int s, int n) {
    char hdr[16];
    int hl = snprintf(hdr, sizeof(hdr), "*%d\r\n", n);
    return sock_write(s, hdr, hl);
}
static int reply_emptyset(int s) {
    return sock_write(s, "*0\r\n", 4);
}

/* ---- Read helpers ---- */

static int recv_line(int s, char *out, size_t max) {
    /* Read one byte at a time until \r\n. Slow but trivial; redis-cli
     * sends one full command per packet so the syscall overhead is small. */
    size_t off = 0;
    while (off + 1 < max) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return -1;
        S.bytes_in++;
        out[off++] = c;
        if (off >= 2 && out[off - 2] == '\r' && out[off - 1] == '\n') {
            out[off - 2] = '\0';
            return (int)(off - 2);
        }
    }
    return -1;
}

static int recv_n(int s, char *out, size_t n) {
    size_t off = 0;
    while (off < n) {
        int r = recv(s, out + off, n - off, 0);
        if (r <= 0) return -1;
        off += r;
        S.bytes_in += r;
    }
    return 0;
}

/* ---- Command parsing ----
 *
 * Returns:
 *   1   ok, *argc/argv populated. Each argv[i] heap-allocated.
 *   0   client disconnected
 *  -1   protocol error (drop client)
 */
typedef struct {
    char  *p;
    size_t len;
} arg_t;

static void args_free(arg_t *args, int n) {
    for (int i = 0; i < n; i++) free(args[i].p);
}

static int read_command(int s, arg_t *args, int *argc_out) {
    char line[CRAW_REDIS_INLINE_MAX];
    int n = recv_line(s, line, sizeof(line));
    if (n <= 0) return n == 0 ? -1 : 0;

    if (line[0] == '*') {
        int argc = atoi(line + 1);
        if (argc <= 0 || argc > CRAW_REDIS_ARGS_MAX) return -1;
        for (int i = 0; i < argc; i++) {
            char hdr[32];
            n = recv_line(s, hdr, sizeof(hdr));
            if (n <= 0 || hdr[0] != '$') {
                args_free(args, i);
                return -1;
            }
            int blen = atoi(hdr + 1);
            if (blen < 0 || blen > CRAW_REDIS_BULK_MAX) {
                args_free(args, i);
                return -1;
            }
            char *p = malloc(blen + 1);
            if (!p) { args_free(args, i); return -1; }
            if (recv_n(s, p, blen) != 0) { free(p); args_free(args, i); return -1; }
            p[blen] = '\0';
            char trail[2];
            if (recv_n(s, trail, 2) != 0) { free(p); args_free(args, i); return -1; }
            args[i].p = p;
            args[i].len = blen;
        }
        *argc_out = argc;
        return 1;
    }

    /* Inline form: split on spaces. */
    int argc = 0;
    char *p = line, *tok;
    while ((tok = strtok_r(p, " ", &p))) {
        if (argc >= CRAW_REDIS_ARGS_MAX) { args_free(args, argc); return -1; }
        size_t l = strlen(tok);
        char *dup = malloc(l + 1);
        if (!dup) { args_free(args, argc); return -1; }
        memcpy(dup, tok, l + 1);
        args[argc].p = dup;
        args[argc].len = l;
        argc++;
    }
    if (argc == 0) {
        /* Bare \r\n — keep-alive ping; reply nothing. */
        *argc_out = 0;
        return 1;
    }
    *argc_out = argc;
    return 1;
}

/* ---- Glob match for KEYS pat ---- */

static bool glob_match(const char *pat, const char *str) {
    while (*pat && *str) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return true;
            while (*str) {
                if (glob_match(pat, str)) return true;
                str++;
            }
            return false;
        }
        if (*pat == '?' || *pat == *str) { pat++; str++; continue; }
        return false;
    }
    while (*pat == '*') pat++;
    return !*pat && !*str;
}

/* ---- LRANGE emit context ---- */

typedef struct {
    int sock;
    int err;
} lrange_ctx_t;

static int lrange_emit(const char *v, size_t len, void *cx) {
    lrange_ctx_t *c = cx;
    if (reply_bulk(c->sock, v, len) != 0) { c->err = -1; return -1; }
    return 0;
}

static int lrange_count(const char *k, int start, int stop) {
    /* Two-pass: count, then stream. We need the count to write the array
     * header before any element. Cheap because list is in-memory. */
    int total = 0;
    if (S.storage->list_llen(k, &total) != 0) return 0;
    if (start < 0) start += total;
    if (stop  < 0) stop  += total;
    if (start < 0) start = 0;
    if (stop >= total) stop = total - 1;
    if (start > stop) return 0;
    return stop - start + 1;
}

/* ---- Command handlers ---- */

#define CMD_IS(s)  (strcasecmp(args[0].p, (s)) == 0)

static int dispatch(int sock, arg_t *args, int argc) {
    S.commands_total++;
    if (argc == 0) return 0;

    /* PING [msg] */
    if (CMD_IS("PING")) {
        if (argc == 1) return reply_simple(sock, "PONG");
        return reply_bulk(sock, args[1].p, args[1].len);
    }

    /* ECHO msg */
    if (CMD_IS("ECHO")) {
        if (argc != 2) return reply_error(sock, "ERR wrong number of arguments for 'echo'");
        return reply_bulk(sock, args[1].p, args[1].len);
    }

    /* QUIT */
    if (CMD_IS("QUIT")) {
        reply_simple(sock, "OK");
        return -2;  /* signal: close client */
    }

    /* SELECT n  (single-DB; only 0 accepted) */
    if (CMD_IS("SELECT")) {
        if (argc != 2) return reply_error(sock, "ERR wrong number of arguments for 'select'");
        int n = atoi(args[1].p);
        if (n != 0) return reply_error(sock, "ERR DB index out of range");
        return reply_simple(sock, "OK");
    }

    /* AUTH pw  (no-op stub for v1; v2 will check NVS slot) */
    if (CMD_IS("AUTH")) return reply_simple(sock, "OK");

    /* CLIENT ... — only minimal subcommands so redis-cli's handshake works */
    if (CMD_IS("CLIENT")) {
        if (argc < 2) return reply_error(sock, "ERR wrong number of arguments for 'client'");
        if (strcasecmp(args[1].p, "GETNAME") == 0) return reply_nil(sock);
        if (strcasecmp(args[1].p, "SETNAME") == 0) return reply_simple(sock, "OK");
        if (strcasecmp(args[1].p, "ID") == 0)      return reply_int(sock, sock);
        return reply_simple(sock, "OK");
    }

    /* COMMAND [COUNT|DOCS] */
    if (CMD_IS("COMMAND")) {
        if (argc >= 2 && strcasecmp(args[1].p, "COUNT") == 0) return reply_int(sock, 22);
        if (argc >= 2 && strcasecmp(args[1].p, "DOCS") == 0)  return reply_emptyset(sock);
        return reply_emptyset(sock);
    }

    /* INFO [section] */
    if (CMD_IS("INFO")) {
        char buf[512];
        int dbsz = S.storage->dbsize();
        int n = snprintf(buf, sizeof(buf),
            "# Server\r\n"
            "redis_version:0.5.0-spore\r\n"
            "redis_mode:standalone\r\n"
            "os:esp32-s3 freertos\r\n"
            "process_id:1\r\n"
            "# Clients\r\n"
            "connected_clients:%d\r\n"
            "# Stats\r\n"
            "total_commands_processed:%llu\r\n"
            "total_net_input_bytes:%llu\r\n"
            "total_net_output_bytes:%llu\r\n"
            "# Keyspace\r\n"
            "db0:keys=%d\r\n",
            S.clients,
            (unsigned long long)S.commands_total,
            (unsigned long long)S.bytes_in,
            (unsigned long long)S.bytes_out,
            dbsz);
        return reply_bulk(sock, buf, n);
    }

    /* DBSIZE */
    if (CMD_IS("DBSIZE")) return reply_int(sock, S.storage->dbsize());

    /* FLUSHDB / FLUSHALL */
    if (CMD_IS("FLUSHDB") || CMD_IS("FLUSHALL")) {
        if (S.storage->flush_all() != 0) return reply_error(sock, "ERR storage flush failed");
        return reply_simple(sock, "OK");
    }

    /* SET k v   (EX/PX/NX/XX not supported in v1) */
    if (CMD_IS("SET")) {
        if (argc < 3) return reply_error(sock, "ERR wrong number of arguments for 'set'");
        int rc = S.storage->str_set(args[1].p, args[2].p, args[2].len);
        if (rc == -2) return reply_error(sock, "ERR value too long");
        if (rc != 0)  return reply_error(sock, "ERR storage write failed");
        return reply_simple(sock, "OK");
    }

    /* GET k */
    if (CMD_IS("GET")) {
        if (argc != 2) return reply_error(sock, "ERR wrong number of arguments for 'get'");
        int kind = S.storage->key_exists(args[1].p);
        if (kind == 2) return reply_error(sock,
            "WRONGTYPE Operation against a key holding the wrong kind of value");
        char buf[CRAW_REDIS_VALUE_MAX];
        size_t len = 0;
        int rc = S.storage->str_get(args[1].p, buf, &len, sizeof(buf));
        if (rc == 1) return reply_nil(sock);
        if (rc != 0) return reply_error(sock, "ERR storage read failed");
        return reply_bulk(sock, buf, len);
    }

    /* DEL k [k...] */
    if (CMD_IS("DEL")) {
        if (argc < 2) return reply_error(sock, "ERR wrong number of arguments for 'del'");
        int n = 0;
        for (int i = 1; i < argc; i++) {
            int rc = S.storage->key_del(args[i].p);
            if (rc > 0) n++;
        }
        return reply_int(sock, n);
    }

    /* EXISTS k [k...] */
    if (CMD_IS("EXISTS")) {
        if (argc < 2) return reply_error(sock, "ERR wrong number of arguments for 'exists'");
        int n = 0;
        for (int i = 1; i < argc; i++) {
            if (S.storage->key_exists(args[i].p) > 0) n++;
        }
        return reply_int(sock, n);
    }

    /* TYPE k */
    if (CMD_IS("TYPE")) {
        if (argc != 2) return reply_error(sock, "ERR wrong number of arguments for 'type'");
        int kind = S.storage->key_exists(args[1].p);
        return reply_simple(sock,
            kind == 1 ? "string" : kind == 2 ? "list" : "none");
    }

    /* KEYS pat */
    if (CMD_IS("KEYS")) {
        if (argc != 2) return reply_error(sock, "ERR wrong number of arguments for 'keys'");
        const char *pat = args[1].p;
        /* Two-pass: collect matching keys, then write array. Bounded by
         * NVS namespace size which is small. */
        char keys[64][CRAW_REDIS_KEY_MAX + 1];
        int n = 0;
        char prev[CRAW_REDIS_KEY_MAX + 1] = "";
        craw_redis_iter_t *it = S.storage->iter_open();
        if (it) {
            char k[CRAW_REDIS_KEY_MAX + 1];
            while (n < 64 && S.storage->iter_next(it, k, sizeof(k)) == 1) {
                if (strcmp(k, prev) == 0) continue;  /* dedupe s+l for same key */
                strncpy(prev, k, sizeof(prev) - 1);
                prev[sizeof(prev) - 1] = '\0';
                if (glob_match(pat, k)) {
                    strncpy(keys[n], k, sizeof(keys[n]) - 1);
                    keys[n][sizeof(keys[n]) - 1] = '\0';
                    n++;
                }
            }
            S.storage->iter_close(it);
        }
        if (reply_array_hdr(sock, n) != 0) return -1;
        for (int i = 0; i < n; i++) reply_bulk(sock, keys[i], strlen(keys[i]));
        return 0;
    }

    /* LPUSH k v [v...] */
    if (CMD_IS("LPUSH") || CMD_IS("RPUSH")) {
        if (argc < 3) return reply_error(sock, "ERR wrong number of arguments");
        int kind = S.storage->key_exists(args[1].p);
        if (kind == 1) return reply_error(sock,
            "WRONGTYPE Operation against a key holding the wrong kind of value");
        bool left = CMD_IS("LPUSH");
        int last_len = 0;
        for (int i = 2; i < argc; i++) {
            int rc = (left ? S.storage->list_lpush : S.storage->list_rpush)
                     (args[1].p, args[i].p, args[i].len, &last_len);
            if (rc == -2) return reply_error(sock, "ERR list size cap reached");
            if (rc != 0)  return reply_error(sock, "ERR storage write failed");
        }
        return reply_int(sock, last_len);
    }

    /* LPOP k */
    if (CMD_IS("LPOP") || CMD_IS("RPOP")) {
        if (argc != 2) return reply_error(sock, "ERR wrong number of arguments");
        int kind = S.storage->key_exists(args[1].p);
        if (kind == 1) return reply_error(sock,
            "WRONGTYPE Operation against a key holding the wrong kind of value");
        char buf[CRAW_REDIS_LIST_ENTRY_MAX];
        size_t len = 0;
        int rc = (CMD_IS("LPOP") ? S.storage->list_lpop : S.storage->list_rpop)
                 (args[1].p, buf, &len, sizeof(buf));
        if (rc == 1) return reply_nil(sock);
        if (rc != 0) return reply_error(sock, "ERR storage read failed");
        return reply_bulk(sock, buf, len);
    }

    /* LLEN k */
    if (CMD_IS("LLEN")) {
        if (argc != 2) return reply_error(sock, "ERR wrong number of arguments");
        int kind = S.storage->key_exists(args[1].p);
        if (kind == 1) return reply_error(sock,
            "WRONGTYPE Operation against a key holding the wrong kind of value");
        int n = 0;
        S.storage->list_llen(args[1].p, &n);
        return reply_int(sock, n);
    }

    /* LINDEX k idx */
    if (CMD_IS("LINDEX")) {
        if (argc != 3) return reply_error(sock, "ERR wrong number of arguments");
        int kind = S.storage->key_exists(args[1].p);
        if (kind == 1) return reply_error(sock,
            "WRONGTYPE Operation against a key holding the wrong kind of value");
        int idx = atoi(args[2].p);
        char buf[CRAW_REDIS_LIST_ENTRY_MAX];
        size_t len = 0;
        int rc = S.storage->list_lindex(args[1].p, idx, buf, &len, sizeof(buf));
        if (rc == 1) return reply_nil(sock);
        if (rc != 0) return reply_error(sock, "ERR storage read failed");
        return reply_bulk(sock, buf, len);
    }

    /* LRANGE k start stop */
    if (CMD_IS("LRANGE")) {
        if (argc != 4) return reply_error(sock, "ERR wrong number of arguments");
        int kind = S.storage->key_exists(args[1].p);
        if (kind == 1) return reply_error(sock,
            "WRONGTYPE Operation against a key holding the wrong kind of value");
        int start = atoi(args[2].p), stop = atoi(args[3].p);
        int count = lrange_count(args[1].p, start, stop);
        if (reply_array_hdr(sock, count) != 0) return -1;
        if (count == 0) return 0;
        lrange_ctx_t cx = { .sock = sock, .err = 0 };
        S.storage->list_lrange(args[1].p, start, stop, lrange_emit, &cx);
        return cx.err;
    }

    char err[128];
    snprintf(err, sizeof(err), "ERR unknown command '%s'", args[0].p);
    return reply_error(sock, err);
}

/* ---- Per-client task ---- */

typedef struct { int sock; struct sockaddr_in peer; } cli_t;

static void client_task(void *arg) {
    cli_t *c = (cli_t *)arg;
    int sock = c->sock;
    char ip[16];
    inet_ntoa_r(c->peer.sin_addr, ip, sizeof(ip));
    uint16_t port = ntohs(c->peer.sin_port);
    free(c);

    ESP_LOGI(TAG, "client + %s:%u", ip, port);
    S.clients++;

    arg_t args[CRAW_REDIS_ARGS_MAX] = {0};
    int argc = 0;
    while (S.running) {
        int r = read_command(sock, args, &argc);
        if (r != 1) break;
        int dr = dispatch(sock, args, argc);
        args_free(args, argc);
        argc = 0;
        if (dr == -2) break;        /* QUIT */
        if (dr < 0)  break;          /* socket error */
    }
    args_free(args, argc);
    close(sock);
    S.clients--;
    ESP_LOGI(TAG, "client - %s:%u", ip, port);
    vTaskDelete(NULL);
}

/* ---- Listener task ---- */

static void listen_task(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        ESP_LOGE(TAG, "socket() failed errno=%d", errno);
        S.running = false; vTaskDelete(NULL); return;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(S.cfg.port);
    if (inet_pton(AF_INET, S.cfg.bind, &addr.sin_addr) <= 0) {
        ESP_LOGE(TAG, "bad bind addr '%s'", S.cfg.bind);
        close(srv); S.running = false; vTaskDelete(NULL); return;
    }
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind %s:%u failed errno=%d", S.cfg.bind, S.cfg.port, errno);
        close(srv); S.running = false; vTaskDelete(NULL); return;
    }
    if (listen(srv, 4) < 0) {
        ESP_LOGE(TAG, "listen failed errno=%d", errno);
        close(srv); S.running = false; vTaskDelete(NULL); return;
    }

    S.listen_sock = srv;
    /* Non-blocking accept w/ 1s timeout so .running can stop us. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool exposed = (strcmp(S.cfg.bind, "127.0.0.1") != 0);
    ESP_LOGI(TAG, "listening on %s:%u%s",
             S.cfg.bind, S.cfg.port,
             exposed ? "  [WARN] exposed on LAN, no encryption" : "");

    while (S.running) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cs = accept(srv, (struct sockaddr *)&peer, &plen);
        if (cs < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        if (S.clients >= CRAW_REDIS_MAX_CLIENTS) {
            const char *busy = "-ERR max clients reached\r\n";
            send(cs, busy, strlen(busy), 0);
            close(cs);
            continue;
        }
        struct timeval ctv = { .tv_sec = 60, .tv_usec = 0 };
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
        cli_t *c = malloc(sizeof(*c));
        if (!c) { close(cs); continue; }
        c->sock = cs; c->peer = peer;
        if (xTaskCreate(client_task, "rds_cli", 6144, c, 5, NULL) != pdPASS) {
            close(cs); free(c);
        }
    }

    close(srv);
    S.listen_sock = -1;
    ESP_LOGI(TAG, "listener stopped");
    vTaskDelete(NULL);
}

/* ---- Public lifecycle ---- */

int craw_redis_server_start(const craw_redis_config_t *cfg,
                            craw_redis_storage_t *storage) {
    if (!storage) return -3;
    if (S.running)  return -1;
    S.cfg = *cfg;
    S.storage = storage;
    S.running = true;
    if (xTaskCreate(listen_task, "rds_listen", 4096, NULL, 5, &S.listen_task) != pdPASS) {
        S.running = false;
        return -2;
    }
    return 0;
}

void craw_redis_server_stop(void) {
    if (!S.running) return;
    S.running = false;
    if (S.listen_sock >= 0) {
        /* Kicks accept() out of its 1s wait. Nothing to do here besides
         * setting .running; the task drains itself. */
    }
    /* Wait briefly for the listener task to exit. */
    for (int i = 0; i < 30 && S.listen_sock >= 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool craw_redis_server_running(void) { return S.running; }

void craw_redis_server_stats(craw_redis_stats_t *out) {
    if (!out) return;
    out->running = S.running;
    strncpy(out->bind, S.cfg.bind, sizeof(out->bind) - 1);
    out->bind[sizeof(out->bind) - 1] = '\0';
    out->port = S.cfg.port;
    out->clients = S.clients;
    out->commands_total = S.commands_total;
    out->bytes_in  = S.bytes_in;
    out->bytes_out = S.bytes_out;
}
