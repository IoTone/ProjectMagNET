#ifndef PORTAL_H
#define PORTAL_H

/* portal — reading a captive portal's "accept and continue" page. Pure C
 * (no IDF), host-testable (tools/test_logic.c).
 *
 * Only plain HTML forms can be handled this way: the Sentinel has no browser,
 * so portals that build their button in JavaScript, or want a room number or
 * an email address, need the phone relay instead (net_portal.c).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORTAL_URL_MAX  384
#define PORTAL_BODY_MAX 768

typedef struct {
    bool post;
    char url[PORTAL_URL_MAX];      /* absolute */
    char body[PORTAL_BODY_MAX];    /* x-www-form-urlencoded; for GET, already in url */
    int  inputs;                   /* fields submitted */
} portal_form_t;

/* Pick the form most likely to be the "accept" button and fill *out with
 * what submitting it sends: hidden and pre-filled fields as they are, every
 * checkbox ticked (terms boxes start unticked), the first radio of each
 * group, and the first named submit button. Returns false when the page has
 * no form, or the chosen form has a field the Sentinel can't fill (a visible
 * empty text, email or password box). */
bool portal_parse_form(const char *html, const char *page_url, portal_form_t *out);

/* A page with no form that only forwards somewhere else:
 * <meta http-equiv="refresh" content="0; url=...">, or a script setting
 * window.location / location.href. */
bool portal_find_redirect(const char *html, const char *page_url, char *out, size_t n);

/* Resolve href against base (absolute, scheme-relative, root-relative,
 * relative, or empty = base itself). */
void portal_resolve(const char *base, const char *href, char *out, size_t n);

/* ---- the Sentinel's own WiFi setup page (net_portal.c) ---- */

/* Field `key` of an x-www-form-urlencoded body, decoded ('+' and %XX).
 * Returns false when the field is absent. */
bool portal_form_value(const char *body, const char *key, char *out, size_t n);

/* `in` made safe to put between HTML tags or in a quoted attribute. */
void portal_html_escape(const char *in, char *out, size_t n);

/* Answer DNS query q[0..qlen) the way a captive portal does: every A
 * question gets `ip`, anything else an empty answer. Returns the reply's
 * length, or -1 for a packet that isn't a single-question query. */
int portal_dns_answer(const uint8_t *q, int qlen, const uint8_t ip[4], uint8_t *out, int cap);

#endif
