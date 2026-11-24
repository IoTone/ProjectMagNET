#ifndef NET_PORTAL_H
#define NET_PORTAL_H

/* net_portal — getting through "click OK" captive portals.
 *
 * After every WiFi join the Sentinel fetches a check page that answers an
 * open internet with an empty 204. Anything else is a portal:
 *
 *   1. Auto-accept: fetch the portal page, submit its form the way a person
 *      clicking "I agree / Connect" would (portal.c), check again.
 *   2. Phone login, when that fails: the Sentinel opens its own WiFi network
 *      and forwards (NAPT) a phone's traffic onto the venue network. The
 *      phone's own "sign in to network" sheet appears; tapping OK there lets
 *      the *Sentinel's* MAC through, because that is all the venue sees.
 *      The login network closes itself once the check page answers 204.
 *
 *   3. WiFi setup, when there is no WiFi to relay (wrong password, network
 *      gone, never set): the same button opens "<name>-setup" with a page at
 *      10.77.0.1 for picking the network and typing its password. DNS
 *      answers everything with 10.77.0.1, so the phone's sign-in sheet opens
 *      it by itself.
 *
 * While online it re-checks when MQTT has been down for a while, which is
 * how an expired portal session shows up.
 */

#include <stdbool.h>

typedef enum {
    NET_OFFLINE,      /* no WiFi */
    NET_CHECKING,
    NET_OPEN,         /* check page answered 204 */
    NET_ACCEPTING,    /* trying the portal's form */
    NET_PORTAL,       /* portal, and auto-accept did not get through */
    NET_RELAY,        /* phone login network is up */
    NET_NO_INTERNET,  /* WiFi up, but the check page could not be reached at all */
    NET_SETUP,        /* WiFi setup network + page are up */
} net_state_t;

/* Called on the net task when someone picks a network on the setup page,
 * before the Sentinel joins it: save the credentials. */
typedef void (*net_wifi_saved_cb_t)(const char *ssid, const char *pass);

/* `name` (e.g. "Sentinel-3cf4") becomes "<name>-login" / "<name>-setup". */
void        net_portal_init(const char *name);
void        net_portal_on_wifi_saved(net_wifi_saved_cb_t cb, const char *saved_ssid);
void        net_portal_saved_ssid(const char *ssid);   /* after a console change */
void        net_portal_wifi(bool up);         /* from the WiFi event handler */
void        net_portal_check(void);           /* re-check now */
/* Point the check at another URL (bench testing against a fake portal);
 * NULL or "" restores the real one. Not saved. */
void        net_portal_set_check_url(const char *url);
const char *net_portal_check_url(void);
/* Start / stop the phone login network. Without WiFi, "start" opens the
 * setup network instead; "stop" closes either. */
void        net_portal_relay(bool on);
void        net_portal_setup(void);           /* setup network, even while on WiFi */
net_state_t net_portal_state(void);
const char *net_portal_state_name(net_state_t s);
const char *net_portal_detail(void);          /* last outcome, for screen and console */
const char *net_portal_ap_ssid(void);
const char *net_portal_ap_pass(void);
int         net_portal_ap_clients(void);
bool        net_portal_online(void);          /* NET_OPEN */
/* Seconds the internet has been open but MQTT down (0 when not stuck). */
int         net_portal_mqtt_stuck_s(void);

#endif
