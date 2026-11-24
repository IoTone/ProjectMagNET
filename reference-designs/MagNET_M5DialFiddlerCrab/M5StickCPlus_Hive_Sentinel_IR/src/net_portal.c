// net_portal — see net_portal.h.

#include "net_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

#include "craw_mqtt.h"
#include "craw_wifi.h"
#include "portal.h"

static const char *TAG = "portal";

// Android's check endpoint: plain HTTP (a portal can only intercept HTTP)
// and an empty 204 when the internet is really there.
#define CHECK_URL        "http://connectivitycheck.gstatic.com/generate_204"
#define BODY_MAX         (12 * 1024)
#define REDIRECTS_MAX    5
#define ACCEPT_STEPS     4
#define RELAY_TIMEOUT_MS (10 * 60 * 1000)
#define RELAY_CHECK_MS   5000
#define STUCK_RECHECK_S  120
#define RELAY_WIFI_GRACE_MS 60000   /* keep the login network through a short WiFi drop */

#define REQ_CHECK   (1u << 0)
#define REQ_RELAY   (1u << 1)
#define REQ_UNRELAY (1u << 2)       /* closes the login or the setup network */
#define REQ_SETUP   (1u << 3)
#define REQ_SAVED   (1u << 4)

#define SETUP_TIMEOUT_MS (10 * 60 * 1000)
#define SCAN_MAX         16

static char               s_check_url[PORTAL_URL_MAX] = CHECK_URL;
static TaskHandle_t       s_task;
static volatile net_state_t s_state = NET_OFFLINE;
static volatile bool      s_wifi;
static char               s_detail[64] = "";
static char               s_ssid[33], s_setup_ssid[33], s_pass[12];
static net_wifi_saved_cb_t s_saved_cb;
static char               s_saved_ssid[33];
static volatile int       s_clients;
static esp_netif_t       *s_ap;
static int64_t            s_open_since_ms, s_mqtt_ok_ms;

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void set_state(net_state_t s, const char *detail) {
    s_state = s;
    if (detail) snprintf(s_detail, sizeof(s_detail), "%s", detail);
    ESP_LOGI(TAG, "%s%s%s", net_portal_state_name(s), detail ? ": " : "", detail ? detail : "");
}

// ---------------------------------------------------------------- HTTP

typedef struct {
    char *body;
    int   len;
    char  location[PORTAL_URL_MAX];
    char  cookies[384];             // "a=1; b=2" — portals often tie the accept to a session
} resp_t;

static void cookie_add(char *jar, size_t cap, const char *set_cookie) {
    const char *semi = strchr(set_cookie, ';');
    int n = semi ? (int)(semi - set_cookie) : (int)strlen(set_cookie);
    if (!memchr(set_cookie, '=', (size_t)n)) return;
    size_t l = strlen(jar);
    snprintf(jar + l, cap - l, "%s%.*s", l ? "; " : "", n, set_cookie);
}

static esp_err_t http_ev(esp_http_client_event_t *e) {
    resp_t *r = e->user_data;
    switch (e->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (!strcasecmp(e->header_key, "Location"))
            snprintf(r->location, sizeof(r->location), "%s", e->header_value);
        else if (!strcasecmp(e->header_key, "Set-Cookie"))
            cookie_add(r->cookies, sizeof(r->cookies), e->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        if (r->body && r->len + e->data_len < BODY_MAX) {
            memcpy(r->body + r->len, e->data, e->data_len);
            r->len += e->data_len;
            r->body[r->len] = 0;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* One request, no automatic redirects (we want to see the Location).
 * Returns the HTTP status, or -1 when the request never got an answer. */
static int fetch(const char *url, const char *post_body, resp_t *r) {
    r->len = 0;
    r->location[0] = 0;
    if (r->body) r->body[0] = 0;
    esp_http_client_config_t c = {
        .url = url,
        .event_handler = http_ev,
        .user_data = r,
        .disable_auto_redirect = true,
        .timeout_ms = 8000,
        .buffer_size = 1536,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return -1;
    // Some portals only serve their page to something that looks like a browser.
    esp_http_client_set_header(h, "User-Agent",
        "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 (KHTML, like Gecko) Mobile Safari/537.36");
    esp_http_client_set_header(h, "Accept", "text/html,*/*");
    if (r->cookies[0]) esp_http_client_set_header(h, "Cookie", r->cookies);
    if (post_body) {
        esp_http_client_set_method(h, HTTP_METHOD_POST);
        esp_http_client_set_header(h, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(h, post_body, (int)strlen(post_body));
    }
    esp_err_t err = esp_http_client_perform(h);
    int status = err == ESP_OK ? esp_http_client_get_status_code(h) : -1;
    esp_http_client_cleanup(h);
    ESP_LOGI(TAG, "%s %s -> %d%s%s", post_body ? "POST" : "GET", url, status,
             r->location[0] ? " ->" : "", r->location);
    return status;
}

/* GET url, following redirects. On return *url holds the final address. */
static int fetch_follow(char *url, size_t n, resp_t *r) {
    static char next[PORTAL_URL_MAX];
    int st = fetch(url, NULL, r);
    for (int i = 0; i < REDIRECTS_MAX && st >= 300 && st < 400 && r->location[0]; i++) {
        portal_resolve(url, r->location, next, sizeof(next));
        snprintf(url, n, "%s", next);
        st = fetch(url, NULL, r);
    }
    return st;
}

typedef enum { CHECK_OPEN, CHECK_PORTAL, CHECK_FAILED } check_t;

/* The check page: 204 = open. A redirect or any page = portal, and
 * *landing is where it sent us. */
static check_t check(resp_t *r, char *landing, size_t n) {
    int st = fetch(s_check_url, NULL, r);
    if (st == 204) return CHECK_OPEN;
    if (st < 0) return CHECK_FAILED;
    if (st >= 300 && st < 400 && r->location[0]) portal_resolve(s_check_url, r->location, landing, n);
    else snprintf(landing, n, "%s", s_check_url); // DNS-hijack portals serve the page in place
    return CHECK_PORTAL;
}

// ---------------------------------------------------------------- auto-accept

/* All of this runs on the one net task; the URL-sized buffers are static
 * because together they overflowed its stack on top of esp_http_client. */
static bool auto_accept(resp_t *r, const char *landing) {
    static char url[PORTAL_URL_MAX], next[PORTAL_URL_MAX], to[PORTAL_URL_MAX], landing2[PORTAL_URL_MAX];
    static portal_form_t f;
    static resp_t probe;
    snprintf(url, sizeof(url), "%s", landing);
    int st = fetch_follow(url, sizeof(url), r);
    for (int step = 0; step < ACCEPT_STEPS; step++) {
        if (st < 0) { set_state(NET_ACCEPTING, "portal page did not load"); return false; }
        if (r->body && portal_parse_form(r->body, url, &f)) {
            ESP_LOGI(TAG, "form %s %s (%d fields)", f.post ? "POST" : "GET", f.url, f.inputs);
            snprintf(next, sizeof(next), "%s", f.url);
            st = fetch(next, f.post ? f.body : NULL, r);
            // Follow where the accept leads; the last page may hold a second form.
            for (int i = 0; i < REDIRECTS_MAX && st >= 300 && st < 400 && r->location[0]; i++) {
                portal_resolve(next, r->location, to, sizeof(to));
                snprintf(next, sizeof(next), "%s", to);
                st = fetch(next, NULL, r);
            }
            snprintf(url, sizeof(url), "%s", next);
            memset(&probe, 0, sizeof(probe));
            memcpy(probe.cookies, r->cookies, sizeof(probe.cookies));
            vTaskDelay(pdMS_TO_TICKS(1500));          // let the gateway open up
            if (check(&probe, landing2, sizeof(landing2)) == CHECK_OPEN) return true;
            continue;
        }
        if (r->body && portal_find_redirect(r->body, url, to, sizeof(to))) {
            snprintf(url, sizeof(url), "%s", to);
            st = fetch_follow(url, sizeof(url), r);
            continue;
        }
        set_state(NET_ACCEPTING, "sign-in page needs a person");
        return false;
    }
    set_state(NET_ACCEPTING, "pressed the button, still not online");
    return false;
}

// ---------------------------------------------------------------- phone relay

static void ap_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED) s_clients++;
    else if (id == WIFI_EVENT_AP_STADISCONNECTED && s_clients > 0) s_clients--;
}

/* The Sentinel's own network, at 10.77.0.1, with a fresh password. `dns`
 * is what DHCP hands phones as their resolver. */
static bool ap_up(const char *ssid, const esp_netif_dns_info_t *dns) {
    if (!s_ap) {
        s_ap = esp_netif_create_default_wifi_ap();
        // Off the usual 192.168.4.x, which venue networks use too.
        esp_netif_ip_info_t ip;
        IP4_ADDR(&ip.ip, 10, 77, 0, 1);
        IP4_ADDR(&ip.gw, 10, 77, 0, 1);
        IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(s_ap);
        esp_netif_set_ip_info(s_ap, &ip);
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, ap_event, NULL, NULL);
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, ap_event, NULL, NULL);
    }
    snprintf(s_pass, sizeof(s_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
    wifi_config_t ap = { 0 };
    ap.ap.ssid_len = strnlen(ssid, sizeof(ap.ap.ssid));        // 32 bytes, no terminator needed
    memcpy(ap.ap.ssid, ssid, ap.ap.ssid_len);
    ap.ap.channel = 6;                                         // only used while the STA is idle
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", s_pass);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    // Bench, 2026-10-26: with modem sleep on, a phone associating to the AP
    // forced a power-save stop that dropped the STA link to the venue
    // (run -> init, reason 1). Turn sleep off first, so nothing changes at
    // join time, and keep the AP at 20 MHz beside the STA's channel.
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) return false;
    esp_netif_dhcps_stop(s_ap);
    if (dns) {
        uint8_t offer = 0x02;                              // OFFER_DNS
        esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
        esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, (esp_netif_dns_info_t *)dns);
    }
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_netif_dhcps_start(s_ap);
    for (int i = 0; i < 30 && !esp_netif_is_netif_up(s_ap); i++) vTaskDelay(pdMS_TO_TICKS(100));
    s_clients = 0;
    return true;
}

static bool relay_start(void) {
    // Hand phones the venue's DNS: portals answer through their own resolver.
    esp_netif_dns_info_t dns;
    esp_netif_t *sta = craw_wifi_get_netif();
    bool have_dns = sta && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK;
    if (!ap_up(s_ssid, have_dns ? &dns : NULL)) {
        set_state(NET_PORTAL, "could not start the login network");
        return false;
    }
    // NAPT binds to the interface's address, so ap_up waited for it.
    esp_err_t nerr = esp_netif_napt_enable(s_ap);
    if (nerr != ESP_OK) {
        ESP_LOGE(TAG, "NAPT enable: %s (ap up=%d)", esp_err_to_name(nerr), esp_netif_is_netif_up(s_ap));
        esp_wifi_set_mode(WIFI_MODE_STA);
        set_state(NET_PORTAL, "could not forward phone traffic");
        return false;
    }
    set_state(NET_RELAY, "join the login network from a phone");
    return true;
}

static void relay_stop(void) {
    if (s_ap) esp_netif_napt_disable(s_ap);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);                  // IDF's default with WiFi on
    s_clients = 0;
}

// ---------------------------------------------------------------- WiFi setup page

/* With no WiFi there is nothing to relay, so the same button opens a setup
 * network instead: a phone joins it, its "sign in" sheet shows our page
 * (every DNS name answers 10.77.0.1, every unknown URL redirects to /), and
 * the person picks the venue network and types its password. */

static craw_wifi_scan_result_t s_scan[SCAN_MAX];
static int            s_nscan;
static httpd_handle_t s_httpd;
static volatile bool  s_dns_run;
static char           s_new_ssid[33], s_new_pass[65];
static char           s_old_ssid[33];          /* shown as "last tried" */

static void dns_task(void *arg) {
    (void)arg;
    static uint8_t q[512], r[512];
    static const uint8_t ip[4] = { 10, 77, 0, 1 };
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    struct timeval tv = { .tv_sec = 1 };
    if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "DNS: no socket");
        if (sock >= 0) close(sock);
        vTaskDelete(NULL);
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        int m = portal_dns_answer(q, n, ip, r, sizeof(r));
        if (m > 0) sendto(sock, r, m, 0, (struct sockaddr *)&from, fl);
    }
    close(sock);
    vTaskDelete(NULL);
}

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>Sentinel WiFi</title>"
    "<style>body{font:16px system-ui,sans-serif;margin:16px;max-width:480px}"
    "label.n{display:flex;gap:10px;align-items:center;padding:10px;border:1px solid #ccc;"
    "border-radius:8px;margin:6px 0}label.n span{flex:1}small{color:#666}"
    "input[type=text],input[type=password]{width:100%;box-sizing:border-box;font-size:16px;padding:10px;margin:4px 0 12px}"
    "button{font-size:17px;padding:12px;width:100%;border-radius:8px;border:0;background:#1565c0;color:#fff}"
    ".msg{padding:10px;border-radius:8px;background:#fff3e0}</style></head><body>";

static esp_err_t send_page(httpd_req_t *req, const char *msg) {
    static char row[640], esc[200];   // 32-byte SSID, every byte an entity
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h2>Sentinel WiFi setup</h2>");
    if (msg) {
        portal_html_escape(msg, esc, sizeof(esc));
        snprintf(row, sizeof(row), "<p class=msg>%s</p>", esc);
        httpd_resp_sendstr_chunk(req, row);
    } else if (s_old_ssid[0]) {
        portal_html_escape(s_old_ssid, esc, sizeof(esc));
        snprintf(row, sizeof(row), "<p><small>Saved network: %s</small></p>", esc);
        httpd_resp_sendstr_chunk(req, row);
    }
    httpd_resp_sendstr_chunk(req, "<form method=post action=/save><p>Pick the network the Sentinel should use:</p>");
    for (int i = 0; i < s_nscan; i++) {
        if (!s_scan[i].ssid[0]) continue;
        bool dup = false;                            // mesh networks show up once per access point
        for (int j = 0; j < i && !dup; j++) dup = !strcmp(s_scan[i].ssid, s_scan[j].ssid);
        if (dup) continue;
        int bars = s_scan[i].rssi > -55 ? 4 : s_scan[i].rssi > -67 ? 3 : s_scan[i].rssi > -78 ? 2 : 1;
        portal_html_escape(s_scan[i].ssid, esc, sizeof(esc));
        snprintf(row, sizeof(row),
                 "<label class=n><input type=radio name=ssid value=\"%s\"%s><span>%s</span>"
                 "<small>%.*s%s</small></label>",
                 esc, strcmp(s_scan[i].ssid, s_old_ssid) ? "" : " checked", esc,
                 bars, "||||", s_scan[i].authmode == WIFI_AUTH_OPEN ? " open" : "");
        httpd_resp_sendstr_chunk(req, row);
    }
    if (!s_nscan) httpd_resp_sendstr_chunk(req, "<p><small>No networks found nearby.</small></p>");
    httpd_resp_sendstr_chunk(req,
        "<p>Not listed (hidden network)? Type its name:</p><input type=text name=other autocapitalize=none>"
        "<p>Password <small>(leave empty for an open network)</small></p>"
        "<input type=password name=pass autocomplete=off>"
        "<button type=submit>Join</button></form>"
        "<p><small>Sign-in (\"click OK\") networks: join here first, then the Sentinel shows "
        "LOGIN NEEDED if it can't accept the page by itself.</small></p></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* Every GET: the page itself at /, a redirect anywhere else, which is what
 * makes a phone's connectivity check open the sign-in sheet. */
static esp_err_t h_get(httpd_req_t *req) {
    if (strcmp(req->uri, "/")) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://10.77.0.1/");
        return httpd_resp_send(req, NULL, 0);
    }
    return send_page(req, NULL);
}

static esp_err_t h_save(httpd_req_t *req) {
    static char body[384], ssid[33], other[33], pass[65], esc[200], msg[320];
    int n = req->content_len < (int)sizeof(body) - 1 ? (int)req->content_len : (int)sizeof(body) - 1, got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, body + got, n - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = 0;
    ssid[0] = other[0] = pass[0] = 0;
    portal_form_value(body, "ssid", ssid, sizeof(ssid));
    portal_form_value(body, "other", other, sizeof(other));
    portal_form_value(body, "pass", pass, sizeof(pass));
    const char *pick = other[0] ? other : ssid;
    if (!pick[0]) return send_page(req, "Pick a network first.");
    if (pass[0] && strlen(pass) < 8) return send_page(req, "WiFi passwords are at least 8 characters.");
    snprintf(s_new_ssid, sizeof(s_new_ssid), "%s", pick);
    snprintf(s_new_pass, sizeof(s_new_pass), "%s", pass);
    portal_html_escape(pick, esc, sizeof(esc));
    snprintf(msg, sizeof(msg), "<h2>Joining %s</h2><p>This network closes now. Watch the Sentinel's screen.</p>", esc);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, msg);
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    xTaskNotify(s_task, REQ_SAVED, eSetBits);
    return ESP_OK;
}

static void setup_stop(void) {
    s_dns_run = false;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    s_clients = 0;
}

static bool setup_start(const char *saved_ssid) {
    snprintf(s_old_ssid, sizeof(s_old_ssid), "%s", saved_ssid ? saved_ssid : "");
    set_state(NET_CHECKING, "looking for networks...");
    craw_wifi_hold(true);                     // its retry scans would drag the AP off channel
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_nscan = craw_wifi_scan(s_scan, SCAN_MAX);
    if (s_nscan < 0) s_nscan = 0;
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4 };
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(10, 77, 0, 1);
    if (!ap_up(s_setup_ssid, &dns)) {
        craw_wifi_hold(false);
        set_state(NET_OFFLINE, "could not start the setup network");
        return false;
    }
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.max_open_sockets = 4;
    c.lru_purge_enable = true;
    c.stack_size = 4096;
    if (httpd_start(&s_httpd, &c) != ESP_OK) {
        setup_stop();
        craw_wifi_hold(false);
        set_state(NET_OFFLINE, "could not start the setup page");
        return false;
    }
    static const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
    static const httpd_uri_t any  = { .uri = "/*",    .method = HTTP_GET,  .handler = h_get };
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &any);
    s_dns_run = true;
    xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "setup network %s up, %d networks seen", s_setup_ssid, s_nscan);
    set_state(NET_SETUP, "join from a phone to pick WiFi");
    return true;
}

// ---------------------------------------------------------------- task

static void run_check(resp_t *r) {
    static char landing[PORTAL_URL_MAX];
    set_state(NET_CHECKING, NULL);
    r->cookies[0] = 0;
    switch (check(r, landing, sizeof(landing))) {
    case CHECK_OPEN:
        set_state(NET_OPEN, "internet open");
        s_open_since_ms = now_ms();
        return;
    case CHECK_FAILED:
        set_state(NET_NO_INTERNET, "check page unreachable");
        return;
    case CHECK_PORTAL:
        set_state(NET_ACCEPTING, "sign-in page found, accepting...");
        if (auto_accept(r, landing)) {
            set_state(NET_OPEN, "portal accepted automatically");
            s_open_since_ms = now_ms();
        } else {
            static char why[64];
            snprintf(why, sizeof(why), "%s", s_detail);
            set_state(NET_PORTAL, why);
        }
        return;
    }
}

static void net_task(void *arg) {
    (void)arg;
    static resp_t r, probe;
    static char landing[PORTAL_URL_MAX];
    r.body = malloc(BODY_MAX);
    int64_t relay_until = 0, last_relay_check = 0, setup_until = 0;
    for (;;) {
        uint32_t req = 0;
        xTaskNotifyWait(0, UINT32_MAX, &req, pdMS_TO_TICKS(1000));
        int64_t now = now_ms();

        if (s_state == NET_SETUP) {
            // WiFi events don't matter here: the STA is held until we leave.
            if (req & REQ_SAVED) {
                vTaskDelay(pdMS_TO_TICKS(1500));             // let the "Joining" page reach the phone
                setup_stop();
                if (s_saved_cb) s_saved_cb(s_new_ssid, s_new_pass);
                craw_wifi_hold(false);
                craw_wifi_connect(s_new_ssid, s_new_pass);
                static char d[64];
                snprintf(d, sizeof(d), "joining %.40s", s_new_ssid);
                set_state(NET_OFFLINE, d);
            } else if (req & REQ_UNRELAY || now > setup_until) {
                setup_stop();
                craw_wifi_hold(false);                       // back to the saved network
                set_state(NET_OFFLINE, req & REQ_UNRELAY ? "setup closed" : "setup timed out");
            }
            continue;
        }
        // Nothing to relay without WiFi, so "login" means "set up WiFi".
        if (req & REQ_SETUP || (req & REQ_RELAY && !s_wifi && s_state != NET_RELAY)) {
            if (s_state == NET_RELAY) relay_stop();
            if (setup_start(s_saved_ssid)) setup_until = now_ms() + SETUP_TIMEOUT_MS;
            continue;
        }

        if (req & REQ_UNRELAY) {
            if (s_state == NET_RELAY) { relay_stop(); set_state(NET_PORTAL, "login network closed"); }
        }
        static int64_t wifi_lost_ms;
        if (!s_wifi) {
            if (!wifi_lost_ms) wifi_lost_ms = now;
            // The phone may be mid-login; craw_wifi is already reconnecting.
            if (s_state == NET_RELAY && now - wifi_lost_ms < RELAY_WIFI_GRACE_MS) continue;
            if (s_state == NET_RELAY) relay_stop();
            if (s_state != NET_OFFLINE) set_state(NET_OFFLINE, "no WiFi");
            continue;
        }
        wifi_lost_ms = 0;
        if (req & REQ_RELAY && s_state != NET_RELAY) {
            if (relay_start()) { relay_until = now + RELAY_TIMEOUT_MS; last_relay_check = now; }
            continue;
        }
        if (s_state == NET_RELAY) {
            // Close as soon as the venue lets us through, or give up after 10 min.
            if (now - last_relay_check >= RELAY_CHECK_MS) {
                last_relay_check = now;
                memset(&probe, 0, sizeof(probe));
                if (check(&probe, landing, sizeof(landing)) == CHECK_OPEN) {
                    vTaskDelay(pdMS_TO_TICKS(3000));      // let the phone's sheet finish
                    relay_stop();
                    set_state(NET_OPEN, "signed in from the phone");
                    s_open_since_ms = now_ms();
                } else if (now > relay_until) {
                    relay_stop();
                    set_state(NET_PORTAL, "login network timed out");
                }
            }
            continue;
        }
        if (req & REQ_CHECK) { run_check(&r); continue; }   // (the relay branch above wins while it is up)

        // An expired portal session looks like MQTT never coming back.
        if (craw_mqtt_is_connected()) s_mqtt_ok_ms = now;
        if (s_state == NET_OPEN && now - s_mqtt_ok_ms > STUCK_RECHECK_S * 1000 &&
            now - s_open_since_ms > STUCK_RECHECK_S * 1000) {
            ESP_LOGW(TAG, "MQTT down %ds with the internet 'open' — checking again", STUCK_RECHECK_S);
            run_check(&r);
            s_mqtt_ok_ms = now;                           // don't hammer the check page
        }
    }
}

// ---------------------------------------------------------------- API

void net_portal_init(const char *name) {
    snprintf(s_ssid, sizeof(s_ssid), "%.26s-login", name);
    snprintf(s_setup_ssid, sizeof(s_setup_ssid), "%.26s-setup", name);
    s_mqtt_ok_ms = now_ms();
    // esp_http_client + DNS + (for https portals) mbedTLS handshakes.
    xTaskCreate(net_task, "net", 8192, NULL, 3, &s_task);
}

void net_portal_wifi(bool up) {
    s_wifi = up;
    if (s_task) xTaskNotify(s_task, up ? REQ_CHECK : 0, eSetBits);
}

void net_portal_set_check_url(const char *url) {
    snprintf(s_check_url, sizeof(s_check_url), "%s", url && url[0] ? url : CHECK_URL);
}
const char *net_portal_check_url(void) { return s_check_url; }

void net_portal_on_wifi_saved(net_wifi_saved_cb_t cb, const char *saved_ssid) {
    s_saved_cb = cb;
    net_portal_saved_ssid(saved_ssid);
}
void net_portal_saved_ssid(const char *ssid) { snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", ssid ? ssid : ""); }
void net_portal_setup(void)    { if (s_task) xTaskNotify(s_task, REQ_SETUP, eSetBits); }
void net_portal_check(void)    { if (s_task) xTaskNotify(s_task, REQ_CHECK, eSetBits); }
void net_portal_relay(bool on) { if (s_task) xTaskNotify(s_task, on ? REQ_RELAY : REQ_UNRELAY, eSetBits); }
net_state_t net_portal_state(void) { return s_state; }
const char *net_portal_detail(void) { return s_detail; }
const char *net_portal_ap_ssid(void) { return s_state == NET_SETUP ? s_setup_ssid : s_ssid; }
const char *net_portal_ap_pass(void) { return s_pass; }
int  net_portal_ap_clients(void) { return s_clients; }
bool net_portal_online(void) { return s_state == NET_OPEN; }

int net_portal_mqtt_stuck_s(void) {
    if (s_state != NET_OPEN || craw_mqtt_is_connected()) return 0;
    int64_t since = s_mqtt_ok_ms > s_open_since_ms ? s_mqtt_ok_ms : s_open_since_ms;
    return (int)((now_ms() - since) / 1000);
}

const char *net_portal_state_name(net_state_t s) {
    switch (s) {
    case NET_OFFLINE:     return "offline";
    case NET_CHECKING:    return "checking";
    case NET_OPEN:        return "open";
    case NET_ACCEPTING:   return "accepting";
    case NET_PORTAL:      return "login needed";
    case NET_RELAY:       return "phone login";
    case NET_NO_INTERNET: return "no internet";
    case NET_SETUP:       return "wifi setup";
    }
    return "?";
}
