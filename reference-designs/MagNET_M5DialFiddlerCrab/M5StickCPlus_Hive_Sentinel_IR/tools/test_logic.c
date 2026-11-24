// Host tests for the pure-C presence + stats logic.
//
//   cc -I../src -o /tmp/test_logic test_logic.c ../src/presence.c ../src/stats.c ../src/activity.c ../src/portal.c
//   /tmp/test_logic            # asserts; prints "ok"
//   /tmp/test_logic weeks      # one "YYYY-MM-DD key" line per day 2020..2035,
//                              # checked against Python's isocalendar() by
//                              # tools/test_weeks.py

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "presence.h"
#include "stats.h"
#include "activity.h"
#include "portal.h"

#define S(x) ((int64_t)(x) * 1000)

static void test_presence(void) {
    presence_t p;
    presence_init(&p, 0, 300, 300, 30);
    assert(presence_step(&p, S(5), true) == PRES_EV_NONE);       // warm-up ignores motion
    assert(presence_step(&p, S(30), true) == PRES_EV_READY);
    assert(presence_step(&p, S(31), true) == PRES_EV_ENTER);
    assert(p.state == PRES_PRESENT);
    assert(presence_step(&p, S(32), true) == PRES_EV_NONE);      // no re-ENTER while present
    // Motion every 60 s: STILL at +300 s, +600 s, never CLEAR.
    int still = 0;
    for (int t = 33; t <= 31 + 620; t++) {
        pres_event_t e = presence_step(&p, S(t), t % 60 == 0);
        assert(e != PRES_EV_CLEAR && e != PRES_EV_ENTER);
        if (e == PRES_EV_STILL) still++;
    }
    assert(still == 2);
    // Last motion at t=600; clear exactly hold_s later.
    pres_event_t e = PRES_EV_NONE;
    int t;
    for (t = 652; e == PRES_EV_NONE; t++) e = presence_step(&p, S(t), false);
    assert(e == PRES_EV_CLEAR && t - 1 == 900);
    assert(presence_step(&p, S(901), true) == PRES_EV_ENTER);   // second entry

    // CLEAR beats STILL when both fall due together (hold == repeat, no motion after entry).
    presence_init(&p, 0, 300, 300, 0);
    assert(presence_step(&p, 0, true) == PRES_EV_ENTER);
    for (t = 1; t < 300; t++) assert(presence_step(&p, S(t), false) == PRES_EV_NONE);
    assert(presence_step(&p, S(300), false) == PRES_EV_CLEAR);
}

static struct tm at(int y, int mo, int d, int h) {
    struct tm lt = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
                     .tm_hour = h, .tm_isdst = -1 };
    mktime(&lt);
    return lt;
}

static void test_stats(void) {
    stats_t s, closed;
    stats_init(&s);
    stats_add_entry(&s);                                  // before the clock is known
    struct tm lt = at(2026, 10, 26, 15);                  // Monday
    assert(stats_roll(&s, &lt, &closed) == 0);            // key 0 adopts, keeps the count
    assert(s.entries[ST_HOUR] == 1 && s.entries[ST_MONTH] == 1);

    stats_add_entry(&s); stats_add_occ(&s, 90);
    lt = at(2026, 10, 26, 16);
    assert(stats_roll(&s, &lt, &closed) == (1u << ST_HOUR));
    assert(closed.entries[ST_HOUR] == 2 && closed.occ_s[ST_HOUR] == 90);
    assert(s.entries[ST_HOUR] == 0 && s.entries[ST_DAY] == 2);

    lt = at(2026, 11, 1, 0);                              // Sunday: same ISO week, new month
    assert(stats_roll(&s, &lt, &closed) == ((1u << ST_HOUR) | (1u << ST_DAY) | (1u << ST_MONTH)));
    assert(s.entries[ST_WEEK] == 2);
    lt = at(2026, 11, 2, 0);                              // Monday: new week
    assert(stats_roll(&s, &lt, &closed) & (1u << ST_WEEK));
    assert(s.entries[ST_WEEK] == 0);

    stats_add_entry(&s);
    stats_reset(&s, 1u << ST_DAY, &lt);
    assert(s.entries[ST_DAY] == 0 && s.entries[ST_WEEK] == 1);
    assert(stats_roll(&s, &lt, &closed) == 0);            // a reset is not a rollover

    char b[24];
    lt = at(2026, 10, 26, 15);
    stats_label(ST_HOUR, stats_key(ST_HOUR, &lt), b, sizeof b);   assert(!strcmp(b, "2026-10-26T15"));
    stats_label(ST_DAY, stats_key(ST_DAY, &lt), b, sizeof b);     assert(!strcmp(b, "2026-10-26"));
    stats_label(ST_WEEK, stats_key(ST_WEEK, &lt), b, sizeof b);   assert(!strcmp(b, "2026-W44"));
    stats_label(ST_MONTH, stats_key(ST_MONTH, &lt), b, sizeof b); assert(!strcmp(b, "2026-10"));
    lt = at(2028, 12, 31, 0);                                      // leap year, last day
    stats_label(ST_DAY, stats_key(ST_DAY, &lt), b, sizeof b);     assert(!strcmp(b, "2028-12-31"));
}

static void test_activity(void) {
    hist_t h;
    hist_init(&h);
    hist_add(&h, 100);                                    // before the clock is known
    assert(hist_advance(&h, 2000000));                    // adopt: no shift
    assert(h.occ_s[HIST_BINS - 1] == 100);
    hist_add(&h, 900);
    assert(h.occ_s[HIST_BINS - 1] == 900);                // capped at the bin length
    assert(!hist_advance(&h, 2000000));
    assert(hist_advance(&h, 2000002));                    // two bins later
    assert(h.occ_s[HIST_BINS - 3] == 900 && h.occ_s[HIST_BINS - 2] == 0 && h.occ_s[HIST_BINS - 1] == 0);
    assert(hist_advance(&h, 2000002 + HIST_BINS));        // a day+ gap clears everything
    for (int i = 0; i < HIST_BINS; i++) assert(h.occ_s[i] == 0);
    hist_add(&h, 5);
    assert(hist_advance(&h, 1999990));                    // clock stepped back: re-key, keep data
    assert(h.occ_s[HIST_BINS - 1] == 5 && h.key == 1999990);

    recent_t r;
    recent_init(&r, 0);
    recent_add(&r, 1000, 50);
    recent_add(&r, 9999, 50);
    assert(r.active_ms[RECENT_BINS - 1] == 100);
    recent_add(&r, 10000, 50);                            // next 10-s bin
    assert(r.active_ms[RECENT_BINS - 2] == 100 && r.active_ms[RECENT_BINS - 1] == 50);
    assert(recent_total_ms(&r) == 150);
    recent_advance(&r, 10000 + 28 * RECENT_BIN_MS);       // the 100 ms bin is now oldest
    assert(r.active_ms[0] == 100 && recent_total_ms(&r) == 150);
    recent_advance(&r, 10000 + 31 * RECENT_BIN_MS);       // ... and both have aged out
    assert(recent_total_ms(&r) == 0);
}

static void test_portal(void) {
    char u[PORTAL_URL_MAX];
    portal_resolve("http://p.example/a/b/page.html?x=1", "/login", u, sizeof u);
    assert(!strcmp(u, "http://p.example/login"));
    portal_resolve("http://p.example/a/b/page.html?x=1", "ok.php", u, sizeof u);
    assert(!strcmp(u, "http://p.example/a/b/ok.php"));
    portal_resolve("https://p.example/a/", "//cdn.example/x", u, sizeof u);
    assert(!strcmp(u, "https://cdn.example/x"));
    portal_resolve("http://p.example", "go", u, sizeof u);
    assert(!strcmp(u, "http://p.example/go"));
    portal_resolve("http://p.example/a/page?x=1", "?y=2", u, sizeof u);
    assert(!strcmp(u, "http://p.example/a/page?y=2"));
    portal_resolve("http://p.example/a/page", "", u, sizeof u);
    assert(!strcmp(u, "http://p.example/a/page"));

    portal_form_t f;
    // Terms box + hidden token + named button, entity in a value.
    const char *accept =
        "<html><body><h1>Free WiFi</h1>"
        "<FORM Method=POST action='/portal/accept'>"
        "<input type=\"hidden\" name=\"token\" value=\"a&amp;b c\">"
        "<input type=\"checkbox\" name=\"tos\"> I agree to the terms"
        "<input type=\"submit\" name=\"go\" value=\"Connect\"></form></body></html>";
    assert(portal_parse_form(accept, "http://10.0.0.1/welcome/index.html", &f));
    assert(f.post && !strcmp(f.url, "http://10.0.0.1/portal/accept"));
    assert(!strcmp(f.body, "token=a%26b+c&tos=on&go=Connect"));

    // Language picker first, the click-through second: pick the second.
    const char *two =
        "<form action=\"/lang\"><select name=l><option>en</select><input type=submit value=Go></form>"
        "<form method=\"post\" action=\"connect\"><input type=hidden name=mac value=aa:bb>"
        "<p>By clicking OK you accept the terms</p><button>OK</button></form>";
    assert(portal_parse_form(two, "http://gw.local/guest/", &f));
    assert(f.post && !strcmp(f.url, "http://gw.local/guest/connect") && !strcmp(f.body, "mac=aa%3Abb"));

    // GET form: fields ride on the URL.
    const char *get = "<form action=\"ok.cgi?s=1\"><input type=hidden name=t value=9>"
                      "<input type=radio name=plan value=free checked><input type=radio name=plan value=paid>"
                      "<input type=submit value=Continue></form>";
    assert(portal_parse_form(get, "http://h/dir/x.html", &f));
    assert(!f.post && !strcmp(f.url, "http://h/dir/ok.cgi?s=1&t=9&plan=free") && !f.body[0]);

    // A real login (empty email box) is not ours to fill.
    const char *email = "<form method=post><input type=email name=email>"
                        "<input type=checkbox name=tos><input type=submit value=Connect></form>";
    assert(!portal_parse_form(email, "http://h/", &f));
    assert(!portal_parse_form("<html>no forms here</html>", "http://h/", &f));

    // Forwarding pages.
    assert(portal_find_redirect("<meta http-equiv=\"refresh\" content=\"0; URL='/splash?id=7'\">",
                                "http://1.1.1.1/x", u, sizeof u));
    assert(!strcmp(u, "http://1.1.1.1/splash?id=7"));
    assert(portal_find_redirect("<script>window.location = \"https://login.example/p\";</script>",
                                "http://h/", u, sizeof u));
    assert(!strcmp(u, "https://login.example/p"));
    assert(!portal_find_redirect("<p>hello</p>", "http://h/", u, sizeof u));
}

static void test_setup_page(void) {
    char v[64];
    const char *body = "ssid=Hotel+Guest%20%26+Co&pass=p%2Bw%3D1&x=";
    assert(portal_form_value(body, "ssid", v, sizeof v) && !strcmp(v, "Hotel Guest & Co"));
    assert(portal_form_value(body, "pass", v, sizeof v) && !strcmp(v, "p+w=1"));
    assert(portal_form_value(body, "x", v, sizeof v) && !v[0]);
    assert(!portal_form_value(body, "ss", v, sizeof v));
    assert(!portal_form_value("assid=1", "ssid", v, sizeof v));
    assert(portal_form_value("ssid=abcdefgh", "ssid", v, 4) && !strcmp(v, "abc"));
    assert(portal_form_value("a=%zz", "a", v, sizeof v) && !strcmp(v, "%zz"));

    portal_html_escape("<b>\"Tom's\" & co</b>", v, sizeof v);
    assert(!strcmp(v, "&lt;b&gt;&quot;Tom&#39;s&quot; &amp; co&lt;/b&gt;"));
    portal_html_escape("a&b", v, 5);                       // never half an entity
    assert(!strcmp(v, "a"));

    // connectivitycheck.gstatic.com, A IN, with an EDNS OPT record after it.
    static const uint8_t qa[] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 1,
        17, 'c','o','n','n','e','c','t','i','v','i','t','y','c','h','e','c','k',
        7, 'g','s','t','a','t','i','c', 3, 'c','o','m', 0, 0, 1, 0, 1,
        0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0 };
    const uint8_t ip[4] = { 10, 77, 0, 1 };
    uint8_t r[128];
    int qend = 12 + 18 + 8 + 4 + 1 + 4;
    int n = portal_dns_answer(qa, sizeof qa, ip, r, sizeof r);
    assert(n == qend + 16);
    assert(r[0] == 0x12 && r[1] == 0x34 && (r[2] & 0x80) && (r[2] & 1) && r[3] == 0x80);
    assert(r[7] == 1 && r[11] == 0);
    assert(r[qend] == 0xC0 && r[qend + 1] == 0x0C && r[qend + 3] == 1 && !memcmp(r + n - 4, ip, 4));
    uint8_t qaaaa[sizeof qa];
    memcpy(qaaaa, qa, sizeof qa);
    qaaaa[qend - 3] = 28;                                  // AAAA: empty answer
    n = portal_dns_answer(qaaaa, sizeof qaaaa, ip, r, sizeof r);
    assert(n == qend && r[7] == 0);
    assert(portal_dns_answer(qa, 20, ip, r, sizeof r) == -1);          // truncated
    assert(portal_dns_answer(qa, sizeof qa, ip, r, 40) == -1);         // no room
    uint8_t resp[sizeof qa];
    memcpy(resp, qa, sizeof qa);
    resp[2] |= 0x80;
    assert(portal_dns_answer(resp, sizeof resp, ip, r, sizeof r) == -1); // not a query
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "weeks")) {
        for (int y = 2020; y <= 2035; y++)
            for (int d = 1; d <= 366; d++) {
                struct tm lt = at(y, 1, d, 12);
                if (lt.tm_year + 1900 != y) break;
                printf("%04d-%02d-%02d %d\n", y, lt.tm_mon + 1, lt.tm_mday,
                       stats_key(ST_WEEK, &lt));
            }
        return 0;
    }
    test_presence();
    test_stats();
    test_activity();
    test_portal();
    test_setup_page();
    puts("ok");
    return 0;
}
