// portal — see portal.h. A deliberately small HTML reader: it walks tags,
// not a DOM, which is enough for the one-form pages captive portals serve.

#include "portal.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- helpers

static bool ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]) || !b[i]) return false;
    return true;
}

static const char *istrstr(const char *h, const char *n) {
    size_t ln = strlen(n);
    for (; *h; h++)
        if (ieq_n(h, n, ln)) return h;
    return NULL;
}

/* Value of attribute `name` inside the tag text [tag, end). Decodes the
 * handful of entities portals actually use. Returns false when absent. */
static bool attr(const char *tag, const char *end, const char *name, char *out, size_t n) {
    size_t ln = strlen(name);
    for (const char *p = tag + 1; p < end; p++) {         // tag[0] is '<'
        if (!isspace((unsigned char)p[-1]) || !ieq_n(p, name, ln)) continue;
        const char *q = p + ln;
        while (q < end && isspace((unsigned char)*q)) q++;
        if (q >= end || *q != '=') {
            if (q < end && (isspace((unsigned char)*q) || *q == '>' || *q == '/')) {
                if (n) out[0] = 0;                      // bare attribute: checked, selected
                return true;
            }
            continue;
        }
        q++;
        while (q < end && isspace((unsigned char)*q)) q++;
        char quote = (*q == '"' || *q == '\'') ? *q++ : 0;
        size_t o = 0;
        while (q < end && (quote ? *q != quote : !isspace((unsigned char)*q) && *q != '>')) {
            char c = *q++;
            if (c == '&') {
                static const struct { const char *e; char c; } ent[] = {
                    { "amp;", '&' }, { "quot;", '"' }, { "#39;", '\'' }, { "apos;", '\'' },
                    { "lt;", '<' }, { "gt;", '>' }, { "#x2F;", '/' }, { "#47;", '/' },
                };
                for (size_t k = 0; k < sizeof(ent) / sizeof(ent[0]); k++) {
                    size_t le = strlen(ent[k].e);
                    if ((size_t)(end - q) >= le && ieq_n(q, ent[k].e, le)) { c = ent[k].c; q += le; break; }
                }
            }
            if (o + 1 < n) out[o++] = c;
        }
        if (n) out[o] = 0;
        return true;
    }
    return false;
}

static void urlenc_append(char *b, size_t cap, const char *s) {
    size_t o = strlen(b);
    for (; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') b[o++] = (char)c;
        else if (c == ' ') b[o++] = '+';
        else o += snprintf(b + o, cap - o, "%%%02X", c);
    }
    b[o] = 0;
}

static void add_field(portal_form_t *f, const char *name, const char *value) {
    if (!name[0]) return;
    size_t o = strlen(f->body);
    if (o && o + 1 < sizeof(f->body)) { f->body[o] = '&'; f->body[o + 1] = 0; }
    urlenc_append(f->body, sizeof(f->body), name);
    size_t e = strlen(f->body);
    if (e + 1 < sizeof(f->body)) { f->body[e] = '='; f->body[e + 1] = 0; }
    urlenc_append(f->body, sizeof(f->body), value);
    f->inputs++;
}

// ---------------------------------------------------------------- URLs

void portal_resolve(const char *base, const char *href, char *out, size_t n) {
    while (*href && isspace((unsigned char)*href)) href++;
    if (!*href || *href == '#') { snprintf(out, n, "%s", base); return; }
    if (ieq_n(href, "http://", 7) || ieq_n(href, "https://", 8)) { snprintf(out, n, "%s", href); return; }

    const char *scheme_end = strstr(base, "://");
    const char *host = scheme_end ? scheme_end + 3 : base;
    const char *path = strchr(host, '/');
    int scheme_len = scheme_end ? (int)(scheme_end - base) : 4;
    int host_len = path ? (int)(path - host) : (int)strlen(host);

    if (href[0] == '/' && href[1] == '/') {                 // //host/path
        snprintf(out, n, "%.*s:%s", scheme_len, scheme_end ? base : "http", href);
    } else if (href[0] == '/') {                            // /path
        snprintf(out, n, "%.*s://%.*s%s", scheme_len, scheme_end ? base : "http",
                 host_len, host, href);
    } else if (href[0] == '?') {                            // ?query on the same path
        const char *q = path ? strchr(path, '?') : NULL;
        int keep = q ? (int)(q - base) : (int)strlen(base);
        snprintf(out, n, "%.*s%s", keep, base, href);
    } else {                                                // relative to the base directory
        const char *q = path ? strpbrk(path, "?#") : NULL;
        const char *stop = q ? q : base + strlen(base);
        const char *slash = NULL;
        for (const char *p = path ? path : stop; p < stop; p++) if (*p == '/') slash = p;
        if (slash) snprintf(out, n, "%.*s%s", (int)(slash + 1 - base), base, href);
        else snprintf(out, n, "%.*s://%.*s/%s", scheme_len, scheme_end ? base : "http",
                      host_len, host, href);
    }
}

// ---------------------------------------------------------------- forms

/* Score how much a form looks like the "accept / connect" button. */
static int form_score(const char *start, const char *end) {
    static const char *const words[] = {
        "accept", "agree", "connect", "continue", "login", "log in", "sign in",
        "start", "terms", "free wi", "get online", "ok",
    };
    int score = 1;
    for (const char *p = start; p < end; p++) {
        if (*p != '<') continue;
        if (ieq_n(p, "<input", 6) || ieq_n(p, "<button", 7)) {
            const char *te = memchr(p, '>', (size_t)(end - p));
            if (!te) break;
            char type[16] = "";
            attr(p, te, "type", type, sizeof(type));
            if (!strcasecmp(type, "checkbox")) score += 3;
            if (!strcasecmp(type, "submit") || !strcasecmp(type, "image") || ieq_n(p, "<button", 7)) score += 2;
            if (!strcasecmp(type, "password")) score -= 4;  // a real login, not a click-through
        }
    }
    for (size_t k = 0; k < sizeof(words) / sizeof(words[0]); k++) {
        const char *w = istrstr(start, words[k]);
        if (w && w < end) score += 1;
    }
    return score;
}

bool portal_parse_form(const char *html, const char *page_url, portal_form_t *out) {
    memset(out, 0, sizeof(*out));
    const char *best = NULL, *best_end = NULL;
    int best_score = 0;
    for (const char *p = istrstr(html, "<form"); p; p = istrstr(p + 5, "<form")) {
        const char *e = istrstr(p, "</form");
        if (!e) e = p + strlen(p);
        int s = form_score(p, e);
        if (s > best_score) { best_score = s; best = p; best_end = e; }
    }
    if (!best) return false;

    const char *te = strchr(best, '>');
    if (!te || te > best_end) return false;
    char action[PORTAL_URL_MAX] = "", method[8] = "";
    attr(best, te, "action", action, sizeof(action));
    attr(best, te, "method", method, sizeof(method));
    out->post = !strcasecmp(method, "post");
    portal_resolve(page_url, action, out->url, sizeof(out->url));

    char seen_radio[8][64];
    int nradio = 0;
    bool have_submit = false;
    for (const char *p = te; p && p < best_end; p = strchr(p + 1, '<')) {
        bool is_input = ieq_n(p, "<input", 6), is_button = ieq_n(p, "<button", 7);
        if (!is_input && !is_button) continue;
        const char *ie = strchr(p, '>');
        if (!ie || ie > best_end) break;
        char type[16] = "", name[64] = "", value[192] = "";
        attr(p, ie, "type", type, sizeof(type));
        attr(p, ie, "name", name, sizeof(name));
        bool has_value = attr(p, ie, "value", value, sizeof(value));
        if (is_button && !type[0]) snprintf(type, sizeof(type), "submit");

        if (!strcasecmp(type, "checkbox")) {
            add_field(out, name, has_value ? value : "on");
        } else if (!strcasecmp(type, "radio")) {
            bool dup = false;
            for (int i = 0; i < nradio; i++) if (!strcmp(seen_radio[i], name)) dup = true;
            if (!dup && nradio < 8) {
                snprintf(seen_radio[nradio++], sizeof(seen_radio[0]), "%s", name);
                add_field(out, name, has_value ? value : "on");
            }
        } else if (!strcasecmp(type, "submit") || !strcasecmp(type, "image")) {
            if (!have_submit && name[0]) add_field(out, name, value);
            have_submit = true;
        } else if (!strcasecmp(type, "reset") || !strcasecmp(type, "file") ||
                   !strcasecmp(type, "button")) {
            continue;
        } else {
            // hidden, text, email, password, tel, ...: a visible field the
            // portal left empty is something only a person can answer.
            bool hidden = !strcasecmp(type, "hidden");
            if (!hidden && !value[0] && name[0]) return false;
            add_field(out, name, value);
        }
    }

    if (!out->post && out->body[0]) {             // GET: fields go on the URL
        size_t o = strlen(out->url);
        snprintf(out->url + o, sizeof(out->url) - o, "%c%s", strchr(out->url, '?') ? '&' : '?',
                 out->body);
        out->body[0] = 0;
    }
    return true;
}

bool portal_find_redirect(const char *html, const char *page_url, char *out, size_t n) {
    for (const char *p = istrstr(html, "<meta"); p; p = istrstr(p + 5, "<meta")) {
        const char *te = strchr(p, '>');
        if (!te) break;
        char equiv[16] = "", content[PORTAL_URL_MAX] = "";
        if (!attr(p, te, "http-equiv", equiv, sizeof(equiv)) || strcasecmp(equiv, "refresh")) continue;
        attr(p, te, "content", content, sizeof(content));
        const char *u = istrstr(content, "url=");
        if (!u) continue;
        u += 4;
        if (*u == '\'' || *u == '"') u++;
        char href[PORTAL_URL_MAX];
        snprintf(href, sizeof(href), "%s", u);
        size_t l = strlen(href);
        if (l && (href[l - 1] == '\'' || href[l - 1] == '"')) href[l - 1] = 0;
        portal_resolve(page_url, href, out, n);
        return true;
    }
    static const char *const js[] = { "location.href", "window.location", "location.replace(",
                                      "document.location" };
    for (size_t k = 0; k < sizeof(js) / sizeof(js[0]); k++) {
        const char *p = istrstr(html, js[k]);
        if (!p) continue;
        const char *q = strpbrk(p + strlen(js[k]), "'\"");
        if (!q) continue;
        const char *e = strchr(q + 1, *q);
        if (!e || e - q > PORTAL_URL_MAX) continue;
        char href[PORTAL_URL_MAX];
        snprintf(href, sizeof(href), "%.*s", (int)(e - q - 1), q + 1);
        portal_resolve(page_url, href, out, n);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- setup page

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool portal_form_value(const char *body, const char *key, char *out, size_t n) {
    size_t lk = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, lk) || p[lk] != '=') continue;
        size_t o = 0;
        for (const char *v = p + lk + 1; *v && *v != '&' && o + 1 < n; v++) {
            int hi, lo;
            if (*v == '+') out[o++] = ' ';
            else if (*v == '%' && (hi = hexval(v[1])) >= 0 && (lo = hexval(v[2])) >= 0) {
                out[o++] = (char)(hi << 4 | lo);
                v += 2;
            } else out[o++] = *v;
        }
        if (n) out[o] = 0;
        return true;
    }
    return false;
}

void portal_html_escape(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && n; in++) {
        const char *rep = *in == '&' ? "&amp;" : *in == '<' ? "&lt;" : *in == '>' ? "&gt;"
                        : *in == '"' ? "&quot;" : *in == '\'' ? "&#39;" : NULL;
        size_t l = rep ? strlen(rep) : 1;
        if (o + l >= n) break;
        if (rep) memcpy(out + o, rep, l);
        else out[o] = *in;
        o += l;
    }
    if (n) out[o] = 0;
}

int portal_dns_answer(const uint8_t *q, int qlen, const uint8_t ip[4], uint8_t *out, int cap) {
    if (qlen < 12 || (q[2] & 0x80) || q[4] != 0 || q[5] != 1) return -1;   // a query, one question
    int i = 12;
    while (i < qlen && q[i]) {
        if (q[i] & 0xC0) return -1;                                         // no pointers in a question
        i += q[i] + 1;
    }
    i += 1 + 4;                                                             // root label, type, class
    if (i > qlen) return -1;
    bool a_in = q[i - 4] == 0 && q[i - 3] == 1 && q[i - 2] == 0 && q[i - 1] == 1;
    int len = i + (a_in ? 16 : 0);
    if (len > cap) return -1;
    memcpy(out, q, (size_t)i);                                              // header + question
    out[2] = 0x84 | (q[2] & 0x01);                                          // response, authoritative, RD copied
    out[3] = 0x80;                                                          // RA, no error
    out[6] = 0; out[7] = a_in ? 1 : 0;                                      // answers
    out[8] = out[9] = out[10] = out[11] = 0;                                // drop authority/additional (EDNS)
    if (a_in) {
        static const uint8_t rr[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4 };  // name -> question, A IN, TTL 30
        memcpy(out + i, rr, sizeof(rr));
        memcpy(out + i + sizeof(rr), ip, 4);
    }
    return len;
}
