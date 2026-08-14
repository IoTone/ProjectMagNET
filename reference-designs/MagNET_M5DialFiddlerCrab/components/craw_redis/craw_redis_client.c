/*
 * craw_redis_client — minimal one-shot client for the on-device REPL.
 *
 * Build a RESP array from an inline command string, send it, drain the
 * reply into a caller buffer. No connection pool, no pipelining — fine
 * for `s" GET foo" redis-do` from the Capsule's Forth REPL.
 */

#include "craw_redis.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "redis_cli";

/* ---- Build RESP array from inline command (whitespace-split) ---- */

static int build_resp(const char *cmd, char *out, size_t max) {
    /* Tokenize on spaces — naive (no quoting). For dev REPL this is
     * fine: complex strings can be sent as raw RESP if needed. */
    char tmp[CRAW_REDIS_INLINE_MAX];
    size_t cl = strlen(cmd);
    if (cl + 1 > sizeof(tmp)) return -1;
    memcpy(tmp, cmd, cl + 1);

    char *toks[CRAW_REDIS_ARGS_MAX];
    int n = 0;
    char *p = tmp, *t;
    while ((t = strtok_r(p, " \t", &p)) && n < CRAW_REDIS_ARGS_MAX) {
        toks[n++] = t;
    }
    if (n == 0) return -1;

    int off = snprintf(out, max, "*%d\r\n", n);
    if (off < 0 || (size_t)off >= max) return -1;
    for (int i = 0; i < n; i++) {
        size_t tlen = strlen(toks[i]);
        int w = snprintf(out + off, max - off, "$%zu\r\n", tlen);
        if (w < 0 || (size_t)(off + w) >= max) return -1;
        off += w;
        if ((size_t)(off + tlen + 2) >= max) return -1;
        memcpy(out + off, toks[i], tlen);
        off += tlen;
        out[off++] = '\r';
        out[off++] = '\n';
    }
    return off;
}

int craw_redis_client_exec(const char *host, uint16_t port,
                           const char *cmd,
                           char *reply, size_t reply_max) {
    if (!host || !cmd || !reply || reply_max < 16) return -1;

    char wire[CRAW_REDIS_REPLY_MAX];
    int wlen = build_resp(cmd, wire, sizeof(wire));
    if (wlen < 0) return -1;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) { close(s); return -1; }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    }

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "connect %s:%u errno=%d", host, port, errno);
        close(s);
        return -1;
    }

    if (send(s, wire, wlen, 0) != wlen) { close(s); return -2; }

    /* Drain whatever comes back within the timeout window. Single recv
     * is sufficient for typical replies; for big LRANGEs we loop until
     * a full second of silence. */
    size_t off = 0;
    while (off + 1 < reply_max) {
        int r = recv(s, reply + off, reply_max - 1 - off, 0);
        if (r <= 0) break;
        off += r;
        /* If reply ends with \r\n and we've seen the expected sentinel,
         * stop. Naive heuristic: stop after first chunk; redis-cli does
         * the same for short replies. */
        if (r < 256) break;
    }
    reply[off] = '\0';
    close(s);
    return off > 0 ? 0 : -3;
}

/* ---- Pretty-printer ---- */

static int parse_one(const char **pp, const char *end,
                     int depth,
                     void (*sink)(const char *, void *), void *ctx);

static void emit_indent(int depth, void (*sink)(const char *, void *), void *ctx) {
    for (int i = 0; i < depth; i++) sink("  ", ctx);
}

static int parse_line(const char **pp, const char *end, char *out, size_t max) {
    const char *p = *pp;
    size_t n = 0;
    while (p < end - 1) {
        if (p[0] == '\r' && p[1] == '\n') {
            out[n] = '\0';
            *pp = p + 2;
            return (int)n;
        }
        if (n + 1 < max) out[n] = *p;
        n++;
        p++;
    }
    return -1;
}

static int parse_one(const char **pp, const char *end, int depth,
                     void (*sink)(const char *, void *), void *ctx) {
    if (*pp >= end) return -1;
    char prefix = **pp;
    (*pp)++;
    char line[128];
    char buf[256];
    int ll = parse_line(pp, end, line, sizeof(line));
    if (ll < 0) return -1;

    switch (prefix) {
        case '+':
            emit_indent(depth, sink, ctx);
            snprintf(buf, sizeof(buf), "%s\n", line);
            sink(buf, ctx);
            return 0;
        case '-':
            emit_indent(depth, sink, ctx);
            snprintf(buf, sizeof(buf), "(error) %s\n", line);
            sink(buf, ctx);
            return 0;
        case ':':
            emit_indent(depth, sink, ctx);
            snprintf(buf, sizeof(buf), "(integer) %s\n", line);
            sink(buf, ctx);
            return 0;
        case '$': {
            int blen = atoi(line);
            emit_indent(depth, sink, ctx);
            if (blen < 0) {
                sink("(nil)\n", ctx);
                return 0;
            }
            sink("\"", ctx);
            if (*pp + blen + 2 > end) return -1;
            /* Print the bulk verbatim. */
            snprintf(buf, sizeof(buf), "%.*s", blen, *pp);
            sink(buf, ctx);
            sink("\"\n", ctx);
            *pp += blen + 2;
            return 0;
        }
        case '*': {
            int n = atoi(line);
            if (n < 0) {
                emit_indent(depth, sink, ctx);
                sink("(nil)\n", ctx);
                return 0;
            }
            if (n == 0) {
                emit_indent(depth, sink, ctx);
                sink("(empty array)\n", ctx);
                return 0;
            }
            for (int i = 0; i < n; i++) {
                char hdr[16];
                emit_indent(depth, sink, ctx);
                snprintf(hdr, sizeof(hdr), "%d) ", i + 1);
                sink(hdr, ctx);
                if (parse_one(pp, end, depth + 1, sink, ctx) != 0) return -1;
            }
            return 0;
        }
    }
    return -1;
}

void craw_redis_pretty_print(const char *reply, size_t reply_len,
                             void (*sink)(const char *, void *),
                             void *ctx) {
    const char *p = reply;
    const char *end = reply + reply_len;
    while (p < end) {
        if (parse_one(&p, end, 0, sink, ctx) != 0) {
            sink("(parse error)\n", ctx);
            return;
        }
    }
}
